#include "compiler/native/target_version_emitter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/function_declarations.h"
#include "compiler/native/function_emitter.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_memory_layout.h"

namespace vela::compiler::native {
namespace {

constexpr unsigned kInlineTargetFunctions = 32;
constexpr unsigned kInlineTargetReturns = 8;

bool CanSeedWideVersion(const ir::Function& function) {
  const auto& result = function.return_type;
  if (!ir::CanAnalyzeSynchronousFunction(function) || result.kind != Kind::Int || result.nullable) {
    return false;
  }
  std::size_t last_array_read = 0;
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions[index];
    if (instruction.opcode == Opcode::GetIndex &&
        function.value_types[instruction.lhs].kind == Kind::Array) {
      const auto& type = function.value_types[instruction.dst];
      if (!type.nullable && type.kind == Kind::Int) {
        last_array_read = index + 1;
      }
    }
    if (ir::IsBackJump(instruction, index) && last_array_read > instruction.index) {
      return true;
    }
  }
  return false;
}

void AddFunctions(NativeTargetGroup& group, const GeneratedFunction& function) {
  const std::array functions{
      function.entry,           function.body,     function.leaf,
      function.leaf_guard,      function.borrowed, function.baseline_body,
      function.baseline_resume, function.resume,   function.recovery,
      function.resume_recovery,
  };
  for (auto* value : functions) {
    if (value != nullptr && !value->isDeclaration()) {
      group.functions.push_back(value);
    }
  }
}

void EmitDispatch(llvm::Module& module, llvm::Function& generic, llvm::Function& wide) {
  auto& context = module.getContext();
  const auto name = generic.getName().str();
  generic.setName(name + "_generic");
  auto* dispatch =
      llvm::Function::Create(generic.getFunctionType(), generic.getLinkage(), name, module);
  dispatch->copyAttributesFrom(&generic);
  dispatch->setCallingConv(generic.getCallingConv());
  // 映像、公开包装及间接调用仍引用同一个规范入口，内部已克隆的调用图不再经过分派。
  generic.replaceAllUsesWith(dispatch);
  Builder builder(llvm::BasicBlock::Create(context, "entry", dispatch));
  auto* select = llvm::BasicBlock::Create(context, "target.select", dispatch);
  auto* portable = llvm::BasicBlock::Create(context, "generic", dispatch);
  auto* optimized = llvm::BasicBlock::Create(context, "v4", dispatch);
  // 空执行复用原入口的边界校验，不能在选择目标前解引用它。
  builder.CreateCondBr(builder.CreateIsNotNull(dispatch->getArg(0)), select, portable);
  builder.SetInsertPoint(select);
  auto* program = LoadProgramData(builder, dispatch->getArg(0));
  auto* address = builder.CreateConstGEP1_64(builder.getInt8Ty(), program,
                                             runtime::NativeMemoryLayout::kProgramHostTarget);
  auto* target =
      builder.CreateLoad(builder.getIntNTy(runtime::NativeMemoryLayout::kHostTargetBits), address);
  builder.CreateCondBr(WideTargetCondition(builder, target), optimized, portable);
  llvm::SmallVector<llvm::Value*, 4> arguments;
  for (auto& argument : dispatch->args()) {
    arguments.push_back(&argument);
  }
  for (const auto& [block, callee] : {std::pair{portable, &generic}, std::pair{optimized, &wide}}) {
    builder.SetInsertPoint(block);
    auto* call = builder.CreateCall(callee, arguments);
    call->setCallingConv(callee->getCallingConv());
    call->setTailCallKind(llvm::CallInst::TCK_Tail);
    builder.CreateRet(call);
  }
}

}  // namespace

void CloneWideVersion(llvm::Function& generic,
                      llvm::Function& wide,
                      llvm::ValueToValueMapTy& values) {
  for (auto& argument : generic.args()) {
    values[&argument] = wide.getArg(argument.getArgNo());
  }
  llvm::SmallVector<llvm::ReturnInst*, kInlineTargetReturns> returns;
  llvm::CloneFunctionInto(&wide, &generic, values, llvm::CloneFunctionChangeType::LocalChangesOnly,
                          returns);
  wide.addFnAttr("target-cpu", "x86-64-v4");
  wide.addFnAttr("target-features", "");
  wide.addFnAttr("prefer-vector-width", "512");
}

llvm::Value* WideTargetCondition(Builder& builder, llvm::Value* target) {
  return builder.CreateICmpEQ(
      target, llvm::ConstantInt::get(target->getType(), static_cast<std::uint64_t>(
                                                            runtime::NativeHostTarget::X86_64V4)));
}

std::vector<NativeTargetGroup> PlanTargetVersions(const ir::Program& program,
                                                  const NativeCallPlan& calls,
                                                  const FunctionDeclarations& declarations) {
  const auto count = program.functions.size();
  std::vector<std::vector<std::size_t>> callers(count);
  std::vector<std::uint8_t> needed(count);
  std::vector<std::size_t> pending;
  for (std::size_t index = 0; index < count; ++index) {
    const auto& function = program.functions[index];
    ForEachDirectCallee(function, calls.direct_calls[index],
                        [&](std::size_t callee) { callers[callee].push_back(index); });
    // 只由非空整数结果的数组循环播种；混合返回值构造不凭数组读取推断宽向量收益。
    if (CanSeedWideVersion(function)) {
      needed[index] = 1;
      pending.push_back(index);
    }
  }
  // 反向传播一次，避免为每个入口重新遍历整个调用图。
  while (!pending.empty()) {
    const auto index = pending.back();
    pending.pop_back();
    for (const auto caller : callers[index]) {
      if (needed[caller] == 0 && ir::CanAnalyzeSynchronousFunction(program.functions[caller])) {
        needed[caller] = 1;
        pending.push_back(caller);
      }
    }
  }
  std::vector<NativeTargetGroup> result(count);
  for (std::size_t index = 0; index < count; ++index) {
    auto& group = result[index];
    if (needed[index] == 0) {
      continue;
    }
    AddFunctions(group, declarations.baseline[index]);
    AddFunctions(group, declarations.optimized[index]);
    const auto& specialized = declarations.specialized;
    if (!specialized.empty()) {
      AddFunctions(group, specialized[index]);
    }
    group.entry = declarations.baseline[index].entry;
  }
  return result;
}

void EmitTargetVersions(llvm::Module& module,
                        std::span<const NativeTargetGroup> groups,
                        std::span<std::vector<std::string>> symbols) {
  struct Version final {
    llvm::Function* generic;
    llvm::Function* wide;
    std::size_t source;
  };
  llvm::SmallPtrSet<llvm::Function*, kInlineTargetFunctions> visited;
  llvm::ValueToValueMapTy values;
  std::vector<Version> versions;
  for (std::size_t index = 0; index < groups.size(); ++index) {
    for (auto* function : groups[index].functions) {
      if (!visited.insert(function).second) {
        continue;
      }
      auto* wide =
          llvm::Function::Create(function->getFunctionType(), llvm::GlobalValue::InternalLinkage,
                                 function->getName() + "_v4", module);
      wide->copyAttributesFrom(function);
      wide->setCallingConv(function->getCallingConv());
      values[function] = wide;
      versions.push_back({.generic = function, .wide = wide, .source = index});
    }
  }
  for (const auto& version : versions) {
    CloneWideVersion(*version.generic, *version.wide, values);
    symbols[version.source].push_back(version.wide->getName().str());
  }
  for (std::size_t index = 0; index < groups.size(); ++index) {
    auto* entry = groups[index].entry;
    if (entry == nullptr || (values.count(entry) == 0U)) {
      continue;
    }
    llvm::Value* mapped = values[entry];
    EmitDispatch(module, *entry, *llvm::cast<llvm::Function>(mapped));
    symbols[index].push_back(entry->getName().str());
  }
}

}  // namespace vela::compiler::native
