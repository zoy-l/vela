#include "compiler/native/host_entry_emitter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/AtomicOrdering.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "base/builtin.h"
#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/native/array_reduction_optimizer.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/memory_regions.h"
#include "compiler/native/native_values.h"
#include "runtime/heap/array_storage.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/native/native_program_state.h"

namespace vela::compiler::native {
namespace {

constexpr std::size_t kMaximumHostSourceInstructions = 128;
constexpr std::size_t kMaximumExpandedHostInstructions = 1024;

bool IsPrimitive(const SourceType& type) {
  return type.kind == Kind::Int || type.kind == Kind::Double || type.kind == Kind::Bool;
}

bool HasPrimitiveFieldWrite(const ir::Program& program,
                            const ir::Function& function,
                            const ir::Instruction& instruction) {
  if (instruction.field_declaration || !IsPrimitive(function.value_types[instruction.lhs])) {
    return false;
  }
  const auto& receiver = function.value_types[instruction.dst];
  const auto found = std::ranges::find_if(
      program.classes, [&](const auto& type) { return type.name == receiver.name; });
  return found != program.classes.end() && instruction.index < found->field_types.size() &&
         IsPrimitive(found->field_types[instruction.index]);
}

bool IsCandidate(const ir::Program& program, const ir::Function& function) {
  if (!function.native_symbol.empty() || !ir::CanAnalyzeSynchronousFunction(function) ||
      !function.handlers.empty() || function.instructions.size() > kMaximumHostSourceInstructions ||
      !IsScalar(function.return_type) || function.return_type.kind == Kind::Enum) {
    return false;
  }
  for (const auto& instruction : function.instructions) {
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
        continue;
      case Opcode::GetField:
        if (instruction.field_declaration)
          return false;
        continue;
      case Opcode::SetField:
        if (!HasPrimitiveFieldWrite(program, function, instruction))
          return false;
        continue;
      default:
        if (ir::IsBuiltinCall(instruction, Builtin::ArrayLength))
          continue;
        const auto effects = ir::AnalyzeEffects(instruction);
        if (effects.MayCollectOnSuccess() || effects.reads_state || effects.writes_state ||
            effects.changes_control || effects.may_suspend) {
          return false;
        }
    }
  }
  return true;
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

llvm::Value* Requests(Builder& builder, llvm::Value* context) {
  auto* control = builder.CreateLoad(
      builder.getPtrTy(), Address(builder, context, runtime::NativeMemoryLayout::kHostControl));
  auto* address = builder.CreateLoad(
      builder.getPtrTy(),
      Address(builder, control, offsetof(runtime::NativeControlState, requests)));
  auto* requests = builder.CreateLoad(builder.getInt8Ty(), address);
  requests->setAtomic(llvm::AtomicOrdering::Monotonic);
  return requests;
}

void RemoveHostEntry(GeneratedFunction& function) {
  auto* selector = function.host_entry_selector;
  if (selector == nullptr) {
    return;
  }
  selector->replaceAllUsesWith(llvm::ConstantPointerNull::get(selector->getType()));
  const std::array entries{selector, function.host_guard, function.host_entry,
                           function.host_wide_entry};
  for (auto* entry : entries) {
    if (entry != nullptr)
      entry->dropAllReferences();
  }
  for (auto* entry : entries) {
    if (entry != nullptr)
      entry->eraseFromParent();
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
    for (auto& argument : function.host_entry->args()) {
      values[&argument] = wide->getArg(argument.getArgNo());
    }
    llvm::SmallVector<llvm::ReturnInst*, 8> returns;
    llvm::CloneFunctionInto(wide, function.host_entry, values,
                            llvm::CloneFunctionChangeType::LocalChangesOnly, returns);
    wide->addFnAttr(kNativeArrayReductionCandidateAttribute);
    wide->addFnAttr("target-cpu", "x86-64-v4");
    wide->addFnAttr("target-features", "");
    wide->addFnAttr("prefer-vector-width", "512");
  }
  auto* selector = function.host_entry_selector;
  Builder builder(llvm::BasicBlock::Create(module.getContext(), "entry", selector));
  llvm::Value* entry = function.host_entry;
  if (wide != nullptr) {
    entry = builder.CreateSelect(
        builder.CreateICmpEQ(selector->getArg(0), builder.getInt32(static_cast<std::uint32_t>(
                                                      runtime::NativeHostTarget::X86_64V4))),
        wide, entry);
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
  auto& context = module.getContext();
  Builder builder(llvm::BasicBlock::Create(context, "entry", entry));
  auto* miss = llvm::BasicBlock::Create(context, "unready", entry);
  Builder unmatched(miss);
  unmatched.CreateRet(unmatched.getInt8(runtime::kNativeHostMiss));
  auto* state = entry->getArg(0);
  auto* stack_status = builder.CreateLoad(
      builder.getInt8Ty(), Address(builder, state, NativeMemoryLayout::kHostStackStatus));
  ReturnOnFailure(builder, *entry, stack_status);
  auto* object = entry->getArg(1);
  auto* requested = llvm::BasicBlock::Create(context, "entry.observe", entry);
  auto* available = llvm::BasicBlock::Create(context, "entry.available", entry);
  builder.CreateCondBr(builder.CreateICmpNE(Requests(builder, state), builder.getInt8(0)),
                       requested, available,
                       llvm::MDBuilder(context).createUnlikelyBranchWeights());
  builder.SetInsertPoint(requested);
  builder.CreateStore(builder.getInt64(runtime::EncodeNativeTracePosition(
                          {.function = static_cast<std::uint32_t>(index), .instruction = 0})),
                      Address(builder, state, NativeMemoryLayout::kHostTrace));
  builder.CreateStore(builder.getInt32(1),
                      Address(builder, state, NativeMemoryLayout::kHostTraceCount));
  builder.CreateRet(builder.getInt8(runtime::kNativeHostEnter));
  builder.SetInsertPoint(available);
  auto* globals_ready = llvm::BasicBlock::Create(context, "globals.ready", entry);
  llvm::Value* ready = builder.getTrue();
  const auto& parameter = program.functions[index].value_types.front();
  const auto array =
      std::ranges::find(program.layouts.arrays, parameter.name, &layout::Array::name);
  if (array != program.layouts.arrays.end() && array->element.kind == layout::Type::Kind::Integer &&
      !array->element.nullable) {
    auto* range = builder.CreateLoad(
        builder.getInt32Ty(), Address(builder, object, NativeMemoryLayout::kArrayIntegerRange));
    range->setMetadata(llvm::LLVMContext::MD_tbaa,
                       NativeMemoryTag(context, MemoryRegion::HeapHeader));
    ready = builder.CreateAnd(
        ready,
        builder.CreateICmpEQ(range, builder.getInt32(static_cast<std::uint32_t>(
                                        runtime::internal::IntegerArrayRange::Nonnegative31))));
  }
  if (!plan.globals[index].empty()) {
    auto* data = builder.CreateLoad(builder.getPtrTy(),
                                    Address(builder, state, NativeMemoryLayout::kHostProgram));
    auto* globals = builder.CreateLoad(
        builder.getPtrTy(), Address(builder, data, NativeMemoryLayout::kProgramGlobalStates));
    for (const auto slot : plan.globals[index]) {
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
  auto* result_type = ValueType(context, program.functions[index].return_type);
  auto* result = allocation.CreateAlloca(result_type);
  std::vector<llvm::Value*> arguments{
      state, object, depth, Address(builder, state, NativeMemoryLayout::kHostTrace), result};
  auto* invoke = llvm::BasicBlock::Create(context, "bounded", entry);
  builder.CreateCondBr(builder.CreateCall(function.host_guard, arguments), invoke, miss);
  builder.SetInsertPoint(invoke);
  auto* status = builder.CreateCall(function.host_leaf, arguments);
  ReturnOnFailure(builder, *entry, status);
  auto* value = builder.CreateLoad(result_type, result);
  builder.CreateStore(EncodeValue(builder, value, program.functions[index].return_type),
                      entry->getArg(2));
  auto* observe = llvm::BasicBlock::Create(context, "return.observe", entry);
  auto* complete = llvm::BasicBlock::Create(context, "returned", entry);
  builder.CreateCondBr(builder.CreateICmpNE(Requests(builder, state), builder.getInt8(0)), observe,
                       complete, llvm::MDBuilder(context).createUnlikelyBranchWeights());
  builder.SetInsertPoint(observe);
  builder.CreateStore(builder.getInt32(1),
                      Address(builder, state, NativeMemoryLayout::kHostTraceCount));
  builder.CreateRet(builder.getInt8(runtime::kNativeHostObserve));
  builder.SetInsertPoint(complete);
  builder.CreateRet(Status(builder, ExecutionStatus::Returned));
}

}  // namespace

HostEntryPlan DeclareHostEntries(llvm::Module& module,
                                 const ir::Program& program,
                                 std::span<GeneratedFunction> baseline,
                                 std::span<GeneratedFunction> optimized) {
  const auto count = program.functions.size();
  HostEntryPlan plan{.needed = std::vector<std::uint8_t>(count),
                     .globals = std::vector<std::vector<std::uint32_t>>(count)};
  std::vector<std::uint8_t> state(count);
  std::vector<std::size_t> costs(count), depths(count);
  std::function<bool(std::size_t)> visit = [&](std::size_t index) {
    if (state[index] != 0)
      return state[index] == 3;
    state[index] = 1;
    const auto& function = program.functions[index];
    if (!IsCandidate(program, function)) {
      state[index] = 2;
      return false;
    }
    auto cost = function.instructions.size();
    std::size_t depth = 1;
    auto& globals = plan.globals[index];
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::GetGlobal)
        globals.push_back(instruction.index);
      if (instruction.opcode != Opcode::Call)
        continue;
      const auto target = instruction.index;
      if (!visit(target) || costs[target] > kMaximumExpandedHostInstructions - cost) {
        state[index] = 2;
        return false;
      }
      cost += costs[target];
      depth = std::max(depth, depths[target] + 1);
      globals.insert(globals.end(), plan.globals[target].begin(), plan.globals[target].end());
    }
    std::ranges::sort(globals);
    globals.erase(std::ranges::unique(globals).begin(), globals.end());
    costs[index] = cost;
    depths[index] = depth;
    state[index] = depth <= runtime::kNativeHostTraceCapacity ? 3 : 2;
    return state[index] == 3;
  };
  std::function<void(std::size_t)> mark = [&](std::size_t index) {
    if (plan.needed[index] != 0)
      return;
    plan.needed[index] = 1;
    for (const auto& instruction : program.functions[index].instructions) {
      if (instruction.opcode == Opcode::Call)
        mark(instruction.index);
    }
  };
  for (std::size_t index = 0; index < count; ++index) {
    if (IsHostRoot(program.functions[index]) && visit(index))
      mark(index);
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (plan.needed[index] == 0)
      continue;
    auto& generated = optimized[index];
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
    generated.host_leaf->addParamAttr(static_cast<unsigned>(parameters.size() - 1),
                                      llvm::Attribute::NoAlias);
    for (std::size_t parameter = 0; parameter < source.parameter_count; ++parameter) {
      const auto& value = source.value_types[parameter];
      if (IsReferenceKind(value.kind) && !value.nullable) {
        generated.host_leaf->addParamAttr(static_cast<unsigned>(parameter + 1),
                                          llvm::Attribute::NonNull);
      }
    }
    if (IsHostRoot(source)) {
      generated.host_guard = llvm::Function::Create(
          llvm::FunctionType::get(llvm::Type::getInt1Ty(module.getContext()), parameters, false),
          llvm::GlobalValue::InternalLinkage, generated.body->getName() + "_host_guard", module);
      generated.host_guard->addFnAttr(llvm::Attribute::AlwaysInline);
      auto* host_type = llvm::FunctionType::get(llvm::Type::getInt8Ty(module.getContext()),
                                                {llvm::PointerType::getUnqual(module.getContext()),
                                                 llvm::PointerType::getUnqual(module.getContext()),
                                                 llvm::PointerType::getUnqual(module.getContext())},
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
          llvm::FunctionType::get(llvm::PointerType::getUnqual(module.getContext()),
                                  {llvm::Type::getInt32Ty(module.getContext())}, false),
          llvm::GlobalValue::InternalLinkage, generated.entry->getName() + "_host_select", module);
    }
    baseline[index].host_leaf = generated.host_leaf;
    baseline[index].host_guard = generated.host_guard;
    baseline[index].host_entry = generated.host_entry;
    baseline[index].host_wide_entry = generated.host_wide_entry;
    baseline[index].host_entry_selector = generated.host_entry_selector;
  }
  return plan;
}

void FunctionEmitter::RecordHostFailure(Builder& builder,
                                        llvm::Value* count,
                                        std::uint64_t position) {
  using runtime::NativeMemoryLayout;
  auto* context = body_->getArg(0);
  auto* trace = builder.CreateStore(builder.getInt64(position),
                                    Address(builder, context, NativeMemoryLayout::kHostTrace));
  trace->setMetadata(llvm::LLVMContext::MD_tbaa,
                     NativeMemoryTag(module_.getContext(), MemoryRegion::Execution));
  builder.CreateStore(count, Address(builder, context, NativeMemoryLayout::kHostTraceCount));
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
  auto* returned = llvm::BasicBlock::Create(module_.getContext(), "host.call.returned", body_);
  builder_.CreateCondBr(builder_.CreateICmpNE(status, Status(builder_, ExecutionStatus::Returned)),
                        failed, returned);
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
  builder_.SetInsertPoint(returned);
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
  llvm::SmallPtrSet<llvm::Function*, 32> leaves, invalid;
  for (auto& function : functions)
    if (function.host_leaf != nullptr)
      leaves.insert(function.host_leaf);
  bool changed;
  do {
    changed = false;
    for (auto* function : leaves) {
      if (invalid.contains(function))
        continue;
      bool unsafe = function->empty();
      for (auto& block : *function)
        for (auto& instruction : block) {
          const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
          if (call == nullptr)
            continue;
          auto* target = call->getCalledFunction();
          unsafe = unsafe || target == nullptr || invalid.contains(target) ||
                   (!target->isIntrinsic() && !leaves.contains(target));
        }
      if (unsafe) {
        invalid.insert(function);
        changed = true;
      }
    }
  } while (changed);
  std::vector<llvm::Function*> removed;
  for (auto& function : functions) {
    if (function.host_leaf == nullptr || !invalid.contains(function.host_leaf))
      continue;
    RemoveHostEntry(function);
    removed.push_back(function.host_leaf);
    function.host_leaf = nullptr;
  }
  for (auto* function : removed)
    function->dropAllReferences();
  for (auto* function : removed)
    function->eraseFromParent();
  for (std::size_t index = 0; index < functions.size(); ++index) {
    auto& function = functions[index];
    if (function.host_entry == nullptr)
      continue;
    // 调用图已经证明无环；只内联该图，不能在这里改写普通入口的优化阶段。
    bool inlined = true;
    for (;;) {
      llvm::CallBase* selected = nullptr;
      for (auto& block : *function.host_leaf)
        for (auto& instruction : block) {
          auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
          if (call != nullptr && leaves.contains(call->getCalledFunction())) {
            selected = call;
            break;
          }
        }
      if (selected == nullptr)
        break;
      llvm::InlineFunctionInfo info;
      if (!llvm::InlineFunction(*selected, info).isSuccess()) {
        inlined = false;
        break;
      }
    }
    // 守卫必须覆盖整个执行图；保留被调用方会漏算其循环预算。
    if (!inlined) {
      RemoveHostEntry(function);
      continue;
    }
    canonical.run(*function.host_leaf, analyses);
    std::vector<unsigned> arrays;
    const auto& source = program.functions[index];
    for (std::size_t parameter = 0; parameter < source.parameter_count; ++parameter) {
      if (source.value_types[parameter].kind == Kind::Array &&
          !source.value_types[parameter].nullable)
        arrays.push_back(static_cast<unsigned>(parameter + 1));
    }
    GeneratedFunction guard{.leaf = function.host_leaf, .leaf_guard = function.host_guard};
    EmitBoundedLeafGuard(guard, arrays);
    const auto* returned =
        llvm::dyn_cast<llvm::ReturnInst>(function.host_guard->getEntryBlock().getTerminator());
    const auto* constant = returned == nullptr
                               ? nullptr
                               : llvm::dyn_cast<llvm::ConstantInt>(returned->getReturnValue());
    if (constant != nullptr && constant->isZero()) {
      RemoveHostEntry(function);
      continue;
    }
    EmitHostWrapper(module, program, index, plan, function);
    EmitHostSelector(module, function);
  }
  do {
    changed = false;
    for (auto& function : functions) {
      auto* leaf = function.host_leaf;
      if (leaf != nullptr && leaf->use_empty()) {
        leaf->eraseFromParent();
        function.host_leaf = nullptr;
        changed = true;
      }
    }
  } while (changed);
}

}  // namespace vela::compiler::native
