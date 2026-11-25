#include "compiler/native/host_entry_emitter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/InlineCost.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "base/builtin.h"
#include "base/callable.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/array_reduction_optimizer.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/function_declarations.h"
#include "compiler/native/function_emitter.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/memory_regions.h"
#include "compiler/native/native_values.h"
#include "compiler/native/target_version_emitter.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/native/native_program_state.h"

namespace vela::compiler::native {
namespace {

constexpr std::size_t kMaximumHostSourceInstructions = 128;
constexpr std::size_t kMaximumExpandedHostInstructions = 1024;
using ClassIndex = absl::flat_hash_map<std::string_view, const ir::Class*>;

bool IsPrimitive(const SourceType& type) {
  return type.kind == Kind::Int || type.kind == Kind::Double || type.kind == Kind::Bool;
}

bool HasPrimitiveFieldWrite(const ClassIndex& classes,
                            const ir::Function& function,
                            const ir::Instruction& instruction) {
  if (instruction.field_declaration || !IsPrimitive(function.value_types[instruction.lhs])) {
    return false;
  }
  const auto& receiver = function.value_types[instruction.dst];
  const auto found = classes.find(receiver.name);
  if (found == classes.end()) {
    return false;
  }
  const auto& fields = found->second->field_types;
  return instruction.index < fields.size() && IsPrimitive(fields[instruction.index]);
}

bool IsCandidate(const ClassIndex& classes, const ir::Function& function) {
  if (!function.native_symbol.empty() || !ir::CanAnalyzeSynchronousFunction(function) ||
      !function.handlers.empty() || function.instructions.size() > kMaximumHostSourceInstructions ||
      !IsScalar(function.return_type) || function.return_type.kind == Kind::Enum) {
    return false;
  }
  return std::ranges::all_of(function.instructions, [&](const auto& instruction) {
    if (instruction.construction || instruction.completes_construction || instruction.handler) {
      return false;
    }
    switch (instruction.opcode) {
      case Opcode::Call:
      case Opcode::GetGlobal:
      case Opcode::GetIndex:
      case Opcode::Return:
      case Opcode::Jump:
      case Opcode::JumpIfFalse:
      case Opcode::JumpIfNull:
        return true;
      case Opcode::GetField:
        return !instruction.field_declaration;
      case Opcode::SetField:
        return HasPrimitiveFieldWrite(classes, function, instruction);
      default:
        if (ir::IsBuiltinCall(instruction, Builtin::ArrayLength)) {
          return true;
        }
        const auto effects = ir::AnalyzeEffects(instruction);
        return !effects.MayCollectOnSuccess() && !effects.reads_state && !effects.writes_state &&
               !effects.changes_control && !effects.may_suspend;
    }
  });
}

bool IsHostRoot(const ir::Function& function) {
  if (!function.is_exported || function.parameter_count != 1 || !function.signature ||
      function.signature->hidden_parameters != 0 || function.signature->parameters.size() != 1) {
    return false;
  }
  const auto& parameter = function.signature->parameters.front();
  const auto& type = function.value_types.front();
  return !parameter.optional && !parameter.rest && type.kind == Kind::Array && !type.nullable;
}

llvm::Value* Address(Builder& builder, llvm::Value* pointer, std::size_t offset) {
  return builder.CreateConstGEP1_64(builder.getInt8Ty(), pointer, offset);
}

void RecordHostTrace(Builder& builder,
                     llvm::Value* context,
                     llvm::Value* count,
                     std::optional<std::uint64_t> position) {
  using runtime::NativeMemoryLayout;
  auto* tag = NativeMemoryTag(builder.getContext(), MemoryRegion::Execution);
  if (position) {
    auto* trace = builder.CreateStore(builder.getInt64(*position),
                                      Address(builder, context, NativeMemoryLayout::kHostTrace));
    trace->setMetadata(llvm::LLVMContext::MD_tbaa, tag);
  }
  // 返回观察沿用叶函数写入的精确返回位置，不能覆盖为入口位置。
  auto* stored =
      builder.CreateStore(count, Address(builder, context, NativeMemoryLayout::kHostTraceCount));
  stored->setMetadata(llvm::LLVMContext::MD_tbaa, tag);
}

void RemoveHostEntry(GeneratedFunction& function) {
  auto* selector = function.host_entry_selector;
  if (selector == nullptr) {
    return;
  }
  selector->replaceAllUsesWith(llvm::ConstantPointerNull::get(selector->getType()));
  const std::array entries{
      selector,
      function.host_guard,
      function.host_entry,
      function.host_wide_entry,
  };
  for (auto* entry : entries) {
    if (entry != nullptr) {
      entry->dropAllReferences();
    }
  }
  for (auto* entry : entries) {
    if (entry != nullptr) {
      entry->eraseFromParent();
    }
  }
  function.host_guard = nullptr;
  function.host_entry = nullptr;
  function.host_wide_entry = nullptr;
  function.host_entry_selector = nullptr;
}

void EmitHostSelector(llvm::Module& module, const GeneratedFunction& function) {
  auto* wide = function.host_wide_entry;
  if (wide != nullptr) {
    llvm::ValueToValueMapTy values;
    CloneWideVersion(*function.host_entry, *wide, values);
    wide->addFnAttr(kNativeArrayReductionCandidateAttribute);
  }
  auto* selector = function.host_entry_selector;
  Builder builder(llvm::BasicBlock::Create(module.getContext(), "entry", selector));
  llvm::Value* entry = function.host_entry;
  if (wide != nullptr) {
    entry = builder.CreateSelect(WideTargetCondition(builder, selector->getArg(0)), wide, entry);
  }
  builder.CreateRet(entry);
}

void EmitHostWrapper(llvm::Module& module,
                     const ir::Program& program,
                     std::size_t index,
                     const HostEntryPlan& plan,
                     const GeneratedFunction& function) {
  using runtime::NativeMemoryLayout;
  auto* entry = function.host_entry;
  const auto& source = program.functions[index];
  const auto& required_globals = plan.globals[index];
  auto& context = module.getContext();
  Builder builder(llvm::BasicBlock::Create(context, "entry", entry));
  auto* miss = llvm::BasicBlock::Create(context, "unready", entry);
  Builder unmatched(miss);
  unmatched.CreateRet(unmatched.getInt8(runtime::kNativeHostMiss));
  auto* state = entry->getArg(0);
  state->addAttr(llvm::Attribute::NonNull);
  auto* object = entry->getArg(1);
  auto* control = builder.CreateLoad(builder.getPtrTy(),
                                     Address(builder, state, NativeMemoryLayout::kHostControl));
  auto* requests = NativeRequestAddress(builder, control);
  auto* requested = llvm::BasicBlock::Create(context, "entry.observe", entry);
  auto* available = llvm::BasicBlock::Create(context, "entry.available", entry);
  builder.CreateCondBr(
      builder.CreateICmpNE(LoadNativeRequestBits(builder, requests), builder.getInt8(0)), requested,
      available, llvm::MDBuilder(context).createUnlikelyBranchWeights());
  builder.SetInsertPoint(requested);
  RecordHostTrace(builder, state, builder.getInt32(1),
                  runtime::EncodeNativeTracePosition(
                      {.function = static_cast<std::uint32_t>(index), .instruction = 0}));
  builder.CreateRet(builder.getInt8(runtime::kNativeHostEnter));
  builder.SetInsertPoint(available);
  auto* globals_ready = llvm::BasicBlock::Create(context, "globals.ready", entry);
  llvm::Value* ready = builder.getTrue();
  const auto& parameter = source.value_types.front();
  const auto array =
      std::ranges::find(program.layouts.arrays, parameter.name, &layout::Array::name);
  if (array != program.layouts.arrays.end() && array->element.kind == layout::Type::Kind::Integer &&
      !array->element.nullable) {
    ready = builder.CreateAnd(ready, HasNonnegative31ArrayRange(builder, object));
  }
  if (!required_globals.empty()) {
    auto* data = builder.CreateLoad(builder.getPtrTy(),
                                    Address(builder, state, NativeMemoryLayout::kHostProgram));
    auto* globals = builder.CreateLoad(
        builder.getPtrTy(), Address(builder, data, NativeMemoryLayout::kProgramGlobalStates));
    for (const auto slot : required_globals) {
      auto* current = builder.CreateLoad(builder.getInt8Ty(), Address(builder, globals, slot));
      ready = builder.CreateAnd(
          ready,
          builder.CreateICmpEQ(current, builder.getInt8(static_cast<std::uint8_t>(
                                            runtime::NativeProgramState::GlobalState::Ready))));
    }
  }
  builder.CreateCondBr(ready, globals_ready, miss);
  builder.SetInsertPoint(globals_ready);
  auto* depth = builder.CreateLoad(builder.getInt64Ty(),
                                   Address(builder, state, NativeMemoryLayout::kHostDepth));
  Builder allocation(&entry->getEntryBlock(), entry->getEntryBlock().begin());
  auto* result_type = ValueType(context, source.return_type);
  auto* result = allocation.CreateAlloca(result_type);
  // 外部入口为返回观察保留追踪首槽；内联的子调用仍可用空指针跳过返回位置。
  auto* trace = builder.CreateConstInBoundsGEP1_64(builder.getInt8Ty(), state,
                                                   NativeMemoryLayout::kHostTrace);
  std::vector<llvm::Value*> arguments{state, object, depth, trace, result};
  auto* invoke = llvm::BasicBlock::Create(context, "bounded", entry);
  builder.CreateCondBr(builder.CreateCall(function.host_guard, arguments), invoke, miss);
  builder.SetInsertPoint(invoke);
  auto* status = builder.CreateCall(function.host_leaf, arguments);
  ReturnOnFailure(builder, *entry, status);
  auto* value = builder.CreateLoad(result_type, result);
  builder.CreateStore(EncodeValue(builder, value, source.return_type), entry->getArg(2));
  auto* observe = llvm::BasicBlock::Create(context, "return.observe", entry);
  auto* complete = llvm::BasicBlock::Create(context, "returned", entry);
  builder.CreateCondBr(
      builder.CreateICmpNE(LoadNativeRequestBits(builder, requests), builder.getInt8(0)), observe,
      complete, llvm::MDBuilder(context).createUnlikelyBranchWeights());
  builder.SetInsertPoint(observe);
  RecordHostTrace(builder, state, builder.getInt32(1), std::nullopt);
  builder.CreateRet(builder.getInt8(runtime::kNativeHostObserve));
  builder.SetInsertPoint(complete);
  builder.CreateRet(Status(builder, ExecutionStatus::Returned));
}

void MarkHostFunctions(const ir::Program& program, HostEntryPlan& plan, std::size_t index) {
  if (plan.needed[index] != 0) {
    return;
  }
  plan.needed[index] = 1;
  ForEachDirectCallee(program.functions[index], {},
                      [&](std::size_t callee) { MarkHostFunctions(program, plan, callee); });
}

HostEntryPlan PlanHostEntryGraph(const ir::Program& program) {
  const auto count = program.functions.size();
  ClassIndex classes;
  classes.reserve(program.classes.size());
  for (const auto& type : program.classes) {
    classes.emplace(type.name, &type);
  }
  HostEntryPlan plan{
      .needed = std::vector<std::uint8_t>(count),
      .globals = std::vector<std::vector<std::uint32_t>>(count),
  };
  std::vector<std::uint8_t> state(count);
  std::vector<std::size_t> costs(count);
  std::vector<std::size_t> depths(count);
  std::function<bool(std::size_t)> visit = [&](std::size_t index) {
    if (state[index] != 0) {
      return state[index] == 3;
    }
    state[index] = 1;
    const auto& function = program.functions[index];
    if (!IsCandidate(classes, function)) {
      state[index] = 2;
      return false;
    }
    auto cost = function.instructions.size();
    std::size_t depth = 1;
    auto& globals = plan.globals[index];
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::GetGlobal) {
        globals.push_back(instruction.index);
      }
      if (instruction.opcode != Opcode::Call) {
        continue;
      }
      const auto target = instruction.index;
      if (!visit(target) || costs[target] > kMaximumExpandedHostInstructions - cost) {
        state[index] = 2;
        return false;
      }
      cost += costs[target];
      depth = std::max(depth, depths[target] + 1);
      const auto& required_globals = plan.globals[target];
      globals.insert(globals.end(), required_globals.begin(), required_globals.end());
    }
    std::ranges::sort(globals);
    globals.erase(std::ranges::unique(globals).begin(), globals.end());
    costs[index] = cost;
    depths[index] = depth;
    state[index] = depth <= runtime::kNativeHostTraceCapacity ? 3 : 2;
    return state[index] == 3;
  };
  for (std::size_t index = 0; index < count; ++index) {
    if (IsHostRoot(program.functions[index]) && visit(index)) {
      MarkHostFunctions(program, plan, index);
    }
  }
  return plan;
}

constexpr unsigned kInlineHostFunctions = 32;
constexpr unsigned kInlineHostCalls = 16;

struct HostLeafGraph final {
  llvm::SmallPtrSet<llvm::Function*, kInlineHostFunctions> leaves;
  llvm::SmallPtrSet<llvm::Function*, kInlineHostFunctions> invalid;
};

bool HasUnsupportedHostCalls(const llvm::Function& function, const HostLeafGraph& graph) {
  return function.empty() ||
         std::ranges::any_of(llvm::instructions(function), [&](const auto& instruction) {
           const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
           if (call == nullptr) {
             return false;
           }
           const auto* target = call->getCalledFunction();
           return target == nullptr || graph.invalid.contains(target) ||
                  (!target->isIntrinsic() && !graph.leaves.contains(target));
         });
}

HostLeafGraph FindHostLeafGraph(std::span<GeneratedFunction> functions) {
  HostLeafGraph graph;
  for (auto& function : functions) {
    if (function.host_leaf != nullptr) {
      graph.leaves.insert(function.host_leaf);
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto* function : graph.leaves) {
      if (!graph.invalid.contains(function) && HasUnsupportedHostCalls(*function, graph)) {
        graph.invalid.insert(function);
        changed = true;
      }
    }
  }
  return graph;
}

void RetireInvalidHostLeaves(std::span<GeneratedFunction> functions, const HostLeafGraph& graph) {
  std::vector<llvm::Function*> removed;
  for (auto& function : functions) {
    if (function.host_leaf == nullptr || !graph.invalid.contains(function.host_leaf)) {
      continue;
    }
    RemoveHostEntry(function);
    removed.push_back(function.host_leaf);
    function.host_leaf = nullptr;
  }
  for (auto* function : removed) {
    function->dropAllReferences();
  }
  for (auto* function : removed) {
    function->eraseFromParent();
  }
}

llvm::InlineResult InlineHostGraph(llvm::Function& function, const HostLeafGraph& graph) {
  llvm::SmallVector<llvm::WeakTrackingVH, kInlineHostCalls> pending;
  for (auto& instruction : llvm::instructions(function)) {
    auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (call != nullptr && graph.leaves.contains(call->getCalledFunction())) {
      pending.push_back(call);
    }
  }
  while (!pending.empty()) {
    auto* selected = llvm::dyn_cast_or_null<llvm::CallBase>(pending.pop_back_val());
    if (selected == nullptr) {
      continue;
    }
    llvm::InlineFunctionInfo info;
    const auto result = llvm::InlineFunction(*selected, info);
    if (!result.isSuccess()) {
      return result;
    }
    for (auto* call : info.InlinedCallSites) {
      if (graph.leaves.contains(call->getCalledFunction())) {
        pending.push_back(call);
      }
    }
  }
  return llvm::InlineResult::success();
}

void PruneUnusedHostLeaves(std::span<GeneratedFunction> functions) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto& function : functions) {
      auto* leaf = function.host_leaf;
      if (leaf != nullptr && leaf->use_empty()) {
        leaf->eraseFromParent();
        function.host_leaf = nullptr;
        changed = true;
      }
    }
  }
}

}  // namespace

HostEntryPlan DeclareHostEntries(llvm::Module& module,
                                 const ir::Program& program,
                                 FunctionDeclarations& declarations) {
  const auto count = program.functions.size();
  auto plan = PlanHostEntryGraph(program);
  for (std::size_t index = 0; index < count; ++index) {
    if (plan.needed[index] == 0) {
      continue;
    }
    auto& generated = declarations.optimized[index];
    const auto& source = program.functions[index];
    auto* type = generated.body->getFunctionType();
    std::vector<llvm::Type*> parameters(type->param_begin(), type->param_end());
    const HostEntryLayout layout(source.parameter_count);
    parameters.insert(parameters.begin() + layout.depth,
                      llvm::Type::getInt64Ty(module.getContext()));
    parameters.insert(parameters.begin() + layout.return_position,
                      llvm::PointerType::getUnqual(module.getContext()));
    generated.host_leaf = llvm::Function::Create(
        llvm::FunctionType::get(type->getReturnType(), parameters, false),
        llvm::GlobalValue::InternalLinkage, generated.body->getName() + "_host", module);
    generated.host_leaf->addFnAttr(llvm::Attribute::AlwaysInline);
    const std::array typed_entries{generated.host_leaf};
    SetTypedArguments(typed_entries, source);
    if (IsHostRoot(source)) {
      generated.host_guard = llvm::Function::Create(
          llvm::FunctionType::get(llvm::Type::getInt1Ty(module.getContext()), parameters, false),
          llvm::GlobalValue::InternalLinkage, generated.body->getName() + "_host_guard", module);
      generated.host_guard->addFnAttr(llvm::Attribute::AlwaysInline);
      auto* host_type =
          llvm::FunctionType::get(llvm::Type::getInt8Ty(module.getContext()),
                                  {
                                      llvm::PointerType::getUnqual(module.getContext()),
                                      llvm::PointerType::getUnqual(module.getContext()),
                                      llvm::PointerType::getUnqual(module.getContext()),
                                  },
                                  false);
      generated.host_entry = llvm::Function::Create(host_type, llvm::GlobalValue::InternalLinkage,
                                                    generated.entry->getName() + "_host", module);
      generated.host_entry->addFnAttr(kNativeArrayReductionCandidateAttribute);
      if (module.getTargetTriple().getArch() == llvm::Triple::x86_64) {
        generated.host_wide_entry =
            llvm::Function::Create(host_type, llvm::GlobalValue::InternalLinkage,
                                   generated.entry->getName() + "_host_v4", module);
      }
      generated.host_entry_selector = llvm::Function::Create(
          llvm::FunctionType::get(
              llvm::PointerType::getUnqual(module.getContext()),
              {
                  llvm::Type::getIntNTy(module.getContext(),
                                        runtime::NativeMemoryLayout::kHostTargetBits),
              },
              false),
          llvm::GlobalValue::InternalLinkage, generated.entry->getName() + "_host_select", module);
      generated.host_entry_selector->addParamAttr(0, llvm::Attribute::ZExt);
    }
    auto& baseline = declarations.baseline[index];
    baseline.host_leaf = generated.host_leaf;
    baseline.host_guard = generated.host_guard;
    baseline.host_entry = generated.host_entry;
    baseline.host_wide_entry = generated.host_wide_entry;
    baseline.host_entry_selector = generated.host_entry_selector;
  }
  return plan;
}

void FunctionEmitter::RecordHostFailure(Builder& builder,
                                        llvm::Value* count,
                                        std::uint64_t position) {
  RecordHostTrace(builder, body_->getArg(0), count, position);
}

void FunctionEmitter::StoreHostReturnPosition() {
  auto* slot = body_->getArg(HostEntryLayout(source_.parameter_count).return_position);
  auto& context = module_.getContext();
  auto* store = llvm::BasicBlock::Create(context, "return.position", body_);
  auto* done = llvm::BasicBlock::Create(context, "return.ready", body_);
  builder_.CreateCondBr(builder_.CreateIsNotNull(slot), store, done);
  builder_.SetInsertPoint(store);
  StoreMemory(builder_.getInt64(TracePosition()), slot, MemoryRegion::Execution);
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
}

void FunctionEmitter::EmitHostLeafCall(const ir::Instruction& instruction,
                                       CallableTarget selected) {
  const auto& callee = program_.functions[selected.function];
  auto arguments = CallArguments(instruction, selected, std::nullopt);
  auto* result = arguments.back();
  const HostEntryLayout layout(callee.parameter_count);
  arguments.insert(arguments.begin() + layout.depth,
                   builder_.CreateAdd(entry_depth_, builder_.getInt64(1)));
  arguments.insert(arguments.begin() + layout.return_position,
                   llvm::ConstantPointerNull::get(builder_.getPtrTy()));
  auto* status = builder_.CreateCall(generated_[selected.function].host_leaf, arguments);
  auto* failed = llvm::BasicBlock::Create(module_.getContext(), "host.call.failed", body_);
  auto* succeeded = llvm::BasicBlock::Create(module_.getContext(), "host.call.returned", body_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(status, Status(builder_, ExecutionStatus::Returned)),
                        succeeded, failed);
  builder_.SetInsertPoint(failed);
  auto* context = body_->getArg(0);
  auto* count_address = ByteAddress(context, runtime::NativeMemoryLayout::kHostTraceCount);
  auto* count = LoadMemory(builder_.getInt32Ty(), count_address, MemoryRegion::Execution);
  auto* trace = builder_.CreateInBoundsGEP(
      builder_.getInt64Ty(), ByteAddress(context, runtime::NativeMemoryLayout::kHostTrace), count);
  StoreMemory(builder_.getInt64(TracePosition()), trace, MemoryRegion::Execution);
  StoreMemory(builder_.CreateAdd(count, builder_.getInt32(1)), count_address,
              MemoryRegion::Execution);
  Finish(status);
  builder_.SetInsertPoint(succeeded);
  StoreCallResult(instruction.dst, callee.return_type, result);
}

void FinalizeHostEntries(llvm::Module& module,
                         const ir::Program& program,
                         const HostEntryPlan& plan,
                         std::span<GeneratedFunction> functions) {
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager analyses;
  llvm::CGSCCAnalysisManager calls;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder passes;
  passes.registerModuleAnalyses(modules);
  passes.registerCGSCCAnalyses(calls);
  passes.registerFunctionAnalyses(analyses);
  passes.registerLoopAnalyses(loops);
  passes.crossRegisterProxies(loops, analyses, calls, modules);
  llvm::FunctionPassManager canonical;
  canonical.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
  canonical.addPass(llvm::InstCombinePass());
  canonical.addPass(llvm::SimplifyCFGPass());
  canonical.addPass(llvm::EarlyCSEPass(true));
  canonical.addPass(llvm::InstCombinePass());
  canonical.addPass(llvm::SimplifyCFGPass());
  const auto graph = FindHostLeafGraph(functions);
  RetireInvalidHostLeaves(functions, graph);
  for (std::size_t index = 0; index < functions.size(); ++index) {
    auto& function = functions[index];
    if (function.host_entry == nullptr) {
      continue;
    }
    // 调用图已经证明无环；只内联该图，不能在这里改写普通入口的优化阶段。
    const auto inlined = InlineHostGraph(*function.host_leaf, graph);
    // 守卫必须覆盖整个执行图；保留被调用方会漏算其循环预算。
    if (!inlined.isSuccess()) {
      RemoveHostEntry(function);
      continue;
    }
    canonical.run(*function.host_leaf, analyses);
    std::vector<unsigned> arrays;
    const auto& source = program.functions[index];
    for (std::size_t parameter = 0; parameter < source.parameter_count; ++parameter) {
      if (source.value_types[parameter].kind == Kind::Array &&
          !source.value_types[parameter].nullable) {
        arrays.push_back(static_cast<unsigned>(parameter + 1));
      }
    }
    GeneratedFunction guard{
        .leaf = function.host_leaf,
        .leaf_guard = function.host_guard,
        .inline_scalar = {},
    };
    EmitBoundedLeafGuard(guard, arrays);
    const auto* returned =
        llvm::cast<llvm::ReturnInst>(function.host_guard->getEntryBlock().getTerminator());
    const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(returned->getReturnValue());
    if (constant != nullptr && constant->isZero()) {
      RemoveHostEntry(function);
      continue;
    }
    EmitHostWrapper(module, program, index, plan, function);
    EmitHostSelector(module, function);
  }
  PruneUnusedHostLeaves(functions);
}

}  // namespace vela::compiler::native
