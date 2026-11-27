#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include "base/callable.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/layout_description.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/compile_options.h"
#include "compiler/native/function_emitter.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/invariant_call_optimizer.h"
#include "compiler/native/memory_regions.h"
#include "compiler/native/native_values.h"
#include "compiler/native/struct_values.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/native/native_symbols.h"
#include "runtime/value/object.h"
namespace vela::compiler::native {
llvm::Value* FunctionEmitter::StructReceiverPlace(const ir::StructReceiver& place) {
  switch (place.kind) {
    case ir::StructReceiver::Kind::Local: {
      if (place.receiver == 0 && struct_receiver_ != nullptr) {
        return struct_receiver_;
      }
      if (IsRecovering()) {
        return BoxSlotStructPlace(builder_, slots_.at(place.receiver), place.type);
      }
      const auto root = roots_.slots.find(place.receiver);
      auto* address =
          root == roots_.slots.end()
              ? static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(builder_.getPtrTy()))
              : RootAddress(root->second);
      return LocalStructPlace(builder_, slots_.at(place.receiver), address, place.type);
    }
    case ir::StructReceiver::Kind::Index:
      return ArrayStructPlace(builder_, Reference(place.receiver), Payload(place.index),
                              place.type);
    case ir::StructReceiver::Kind::Global:
      return program_.native_mode == NativeProgramMode::Closed
                 ? BoxSlotStructPlace(builder_, GlobalValueAddress(place.slot), place.type)
                 : GlobalStructPlace(builder_, ProgramData(), place.slot, place.type);
    case ir::StructReceiver::Kind::Field: {
      const auto& receiver_type = source_.value_types.at(place.receiver);
      const auto* owner = FindClass(receiver_type);
      const auto slot = owner == nullptr ? place.slot : ir::PhysicalFieldOffset(*owner, place.slot);
      return owner != nullptr && owner->kind == ir::Class::Kind::CaptureCell
                 ? CapturedStructPlace(builder_, Reference(place.receiver), place.type)
                 : ObjectFieldStructPlace(builder_, Reference(place.receiver), place.type, slot,
                                          place.declaration);
    }
  }
  std::unreachable();
}

llvm::Value* FunctionEmitter::StructCallReceiver(const ir::Instruction& instruction,
                                                 ir::RegisterId reg,
                                                 const SourceType& type) {
  if (instruction.struct_receiver) {
    const auto& place = *instruction.struct_receiver;
    auto* descriptor = StructReceiverPlace(place);
    auto owner = place.type;
    for (const auto field : place.fields) {
      if (owner.nullable) {
        descriptor = RequiredStructPlace(builder_, descriptor, owner);
        owner.nullable = false;
      }
      descriptor = ProjectStructPlace(builder_, descriptor, owner, field);
      owner = owner.value_layout->fields.at(field);
    }
    if (owner.nullable) {
      descriptor = RequiredStructPlace(builder_, descriptor, owner);
    }
    return descriptor;
  }
  const auto& source_type = source_.value_types.at(reg);
  if (source_type.value_layout == nullptr) {
    return BoxedStructPlace(builder_, Reference(reg), type);
  }
  auto* value = ConvertValue(Load(reg), source_type, type);
  Builder allocation(&body_->getEntryBlock(), body_->getEntryBlock().begin());
  auto* storage = allocation.CreateAlloca(ValueType(module_.getContext(), type));
  builder_.CreateStore(value, storage);
  return LocalStructPlace(builder_, storage, llvm::ConstantPointerNull::get(builder_.getPtrTy()),
                          type);
}

FunctionEmitter::CallSelection FunctionEmitter::SelectCallEntry(CallableTarget target,
                                                                std::size_t instruction) const {
  const auto borrowed = escape_.borrowed_calls.empty() ? std::optional<std::size_t>{}
                                                       : escape_.borrowed_calls.at(instruction);
  if (borrowed) {
    // borrowed 入口接收标量字段地址记录；普通叶入口要求完整对象，二者不能互换。
    return {CallEntry::Borrowed, borrowed};
  }
  const auto callee = target.function;
  if (tier_ == NativeCodeTier::Optimized &&
      calls_.IsEntrySlotRequired({.caller = function_index_, .callee = callee})) {
    return {CallEntry::Slot, std::nullopt};
  }
  // 优化编译将 leaf 同时挂到基线声明；恢复体仍须按基线 tier 选择完整入口。
  if (generated_[callee].leaf != nullptr &&
      (program_.native_mode == NativeProgramMode::Closed || tier_ == NativeCodeTier::Optimized)) {
    return {CallEntry::Leaf, std::nullopt};
  }
  if (HasRecovery() && generated_[callee].scalar_field_body != nullptr) {
    return {CallEntry::ScalarField, std::nullopt};
  }
  return {CallEntry::Body, std::nullopt};
}

llvm::Value* FunctionEmitter::EmitScalarFieldCall(CallableTarget selected,
                                                  std::span<llvm::Value* const> arguments) {
  using runtime::NativeMemoryLayout;
  auto& context = module_.getContext();
  const auto& callee = program_.functions[selected.function];
  const auto& target = generated_[selected.function];
  auto* inspect = llvm::BasicBlock::Create(context, "call.field.inspect", body_);
  auto* observable = llvm::BasicBlock::Create(context, "call.field.observable", body_);
  auto* merge = llvm::BasicBlock::Create(context, "call.field.done", body_);
  // 调用边界已保存根和恢复位置；请求观察仍进入完整被调帧，成功片段可按预算分块。
  auto* poll = builder_.CreateCondBr(
      builder_.CreateICmpEQ(LoadSafepointRequests(), builder_.getInt8(0)), inspect, observable);
  if (roots_.bounded_polls) {
    poll->setMetadata(kNativeBoundedPollMetadata, llvm::MDNode::get(context, {}));
  }
  builder_.SetInsertPoint(inspect);
  const auto& signature = callee.signature;
  if (!signature || !signature->receiver) {
    std::unreachable();
  }
  const auto receiver_type = *signature->receiver;
  const auto owner = program_.layouts.types[receiver_type].index;
  auto* table = builder_.CreateLoad(
      builder_.getPtrTy(),
      ByteAddress(ProgramData(), NativeMemoryLayout::kProgramDirectFieldLayouts));
  MarkInvariant(*table);
  auto* expected = builder_.CreateLoad(
      builder_.getPtrTy(), builder_.CreateConstGEP1_64(builder_.getPtrTy(), table, owner));
  MarkInvariant(*expected);
  GuardReadyFields({.object = arguments[1], .expected = expected});
  auto* computed = builder_.CreateCall(target.scalar_field_body, arguments);
  builder_.CreateBr(merge);
  auto* fast_return = builder_.GetInsertBlock();
  builder_.SetInsertPoint(observable);
  // 观察路径保留完整被调帧，且不把完整状态机再次内联进无请求片段。
  auto* called = builder_.CreateCall(target.body, arguments);
  called->addFnAttr(llvm::Attribute::NoInline);
  MarkGuardFailure(*called);
  builder_.CreateBr(merge);
  auto* observed_return = builder_.GetInsertBlock();
  builder_.SetInsertPoint(merge);
  auto* status = builder_.CreatePHI(builder_.getInt8Ty(), 2);
  status->addIncoming(computed, fast_return);
  status->addIncoming(called, observed_return);
  return status;
}

void FunctionEmitter::MarkClosedInvariantCall(llvm::CondBrInst& branch) const {
  if (IsClosedLinkedProgram(program_.native_mode, placement_) && roots_.bounded_polls) {
    MarkInvariantCall(branch);
  }
}

std::optional<CallableTarget> FunctionEmitter::CallTarget(std::size_t index) const {
  const auto& instruction = source_.instructions.at(index);
  if (instruction.opcode == Opcode::Call) {
    return CallableTarget{.function = instruction.index};
  }
  return direct_calls_.empty() ? std::nullopt : direct_calls_.at(index);
}

llvm::BasicBlock* FunctionEmitter::EmitIntrinsicCall(const ir::Instruction& instruction,
                                                     std::uint32_t target) {
  const auto& callee = program_.functions.at(target);
  llvm::BasicBlock* intrinsic_done = nullptr;
  const auto intrinsic = generated_[target].math_intrinsic;
  if (intrinsic != runtime::NativeFunction::Intrinsic::None) {
    std::vector<llvm::Value*> operands;
    operands.reserve(callee.parameter_count);
    for (std::size_t index = 0; index < callee.parameter_count; ++index) {
      operands.push_back(Payload(static_cast<ir::RegisterId>(instruction.lhs + index)));
    }
    auto* matches = IntrinsicCondition(*imports_.at(target), operands.front(), intrinsic);
    if (program_.native_mode == NativeProgramMode::Reloadable || roots_.debug_values) {
      matches = builder_.CreateAnd(
          matches, builder_.CreateICmpEQ(LoadSafepointRequests(), builder_.getInt8(0)));
    }
    auto& context = module_.getContext();
    auto* calculate = llvm::BasicBlock::Create(context, "call.math", body_);
    auto* bound = llvm::BasicBlock::Create(context, "call.binding", body_);
    intrinsic_done = llvm::BasicBlock::Create(context, "call.math.done", body_);
    auto* branch = builder_.CreateCondBr(matches, calculate, bound);
    MarkClosedInvariantCall(*branch);
    builder_.SetInsertPoint(calculate);
    Store(instruction.dst, EmitMathIntrinsic(intrinsic, operands), callee.return_type);
    builder_.CreateBr(intrinsic_done);
    builder_.SetInsertPoint(bound);
    PrepareInlineRuntimeCall();
  }
  return intrinsic_done;
}

std::vector<llvm::Value*> FunctionEmitter::CallArguments(const ir::Instruction& instruction,
                                                         CallableTarget selected,
                                                         std::optional<std::size_t> borrowed) {
  const auto& callee = program_.functions.at(selected.function);
  std::vector<llvm::Value*> arguments{body_->getArg(0)};
  const auto closure = selected.mode == CallableMode::Closure;
  const auto bound = selected.mode == CallableMode::BoundMethod;
  for (std::size_t index = 0; index < callee.parameter_count; ++index) {
    if (index == 0 && borrowed) {
      arguments.push_back(records_.at(*borrowed));
      continue;
    }
    if (bound && index == 0) {
      // 目标来自当前函数中单次定义的 callable；第一个字段是已保根的固定 receiver。
      auto* receiver = LoadMemory(
          builder_.getPtrTy(), ByteAddress(Reference(instruction.rhs), sizeof(runtime::HeapObject)),
          MemoryRegion::FieldPayloads);
      arguments.push_back(callee.struct_receiver
                              ? BoxedStructPlace(builder_, receiver, callee.value_types.front())
                              : receiver);
      continue;
    }
    const auto reg =
        closure && index == 0
            ? instruction.rhs
            : static_cast<ir::RegisterId>(instruction.lhs + index - ((closure || bound) ? 1 : 0));
    const auto& source_type = source_.value_types.at(reg);
    const auto& target_type = callee.value_types.at(index);
    if (index == 0 && callee.struct_receiver) {
      arguments.push_back(StructCallReceiver(instruction, reg, target_type));
      continue;
    }
    auto* value = Load(reg);
    llvm::Value* argument = nullptr;
    if (source_type.value_layout != nullptr && target_type.value_layout == nullptr) {
      // 后续实参装箱可能收集；擦除后的实参在进入被调帧前由暂存区保根。
      auto* encoded = EncodeChecked(value, source_type);
      Pack(encoded, index);
      argument = DecodeValue(builder_, encoded, target_type);
    } else {
      argument = ConvertValue(value, source_type, target_type);
    }
    arguments.push_back(argument);
  }
  if (!callee.native_symbol.empty()) {
    arguments.push_back(imports_.at(selected.function));
  }
  // 帧计划按所有调用的最大返回形状预留暂存区，包含超过 Value ABI 的结构体。
  arguments.push_back(result_);
  return arguments;
}

void FunctionEmitter::StoreCallResult(ir::RegisterId reg,
                                      const SourceType& type,
                                      llvm::Value* address) {
  if (type.kind == Kind::Never) {
    return;
  }
  if (type.kind == Kind::Void) {
    Store(reg, builder_.getFalse(), type);
  } else {
    Store(reg, builder_.CreateLoad(ValueType(module_.getContext(), type), address), type);
  }
}

void FunctionEmitter::EmitLeafCall(const ir::Instruction& instruction,
                                   CallableTarget selected,
                                   std::span<llvm::Value* const> arguments) {
  const auto& target = generated_[selected.function];
  const auto& callee = program_.functions.at(selected.function);
  auto* result = arguments.back();
  auto& context = module_.getContext();
  auto* leaf = llvm::BasicBlock::Create(context, "call.leaf", body_);
  auto* observable = llvm::BasicBlock::Create(context, "call.observable", body_);
  auto* merge = llvm::BasicBlock::Create(context, "call.done", body_);
  auto* failed = llvm::BasicBlock::Create(context, "call.leaf.failed", body_);
  auto* returned = llvm::BasicBlock::Create(context, "call.leaf.returned", body_);
  std::vector<llvm::Value*> leaf_arguments(arguments.begin(), arguments.end());
  const LeafEntryLayout layout(callee.parameter_count);
  // 调用者已经建立自己的逻辑帧；叶入口直接消费其深度，避免在循环中重新读取执行状态。
  leaf_arguments.insert(leaf_arguments.begin() + layout.depth,
                        builder_.CreateAdd(entry_depth_, builder_.getInt64(1)));
  // 调试器及控制请求需要真正的被调用帧；无请求时只执行有界叶片段。
  llvm::Value* ready = builder_.CreateICmpEQ(LoadSafepointRequests(), builder_.getInt8(0));
  if (target.budgeted_recursion) {
    // 纯计算最多展开固定深度；一次预留逻辑和物理栈预算，内层不维护可观察帧。
    auto* depth = builder_.CreateAdd(entry_depth_, builder_.getInt64(kRecursiveLeafDepth + 1));
    ready = builder_.CreateAnd(
        ready, HasStackBudget(depth, kMaximumNativeStackBytes * kRecursiveLeafDepth));
  }
  if (target.leaf_guard != nullptr) {
    ready = builder_.CreateAnd(ready, builder_.CreateCall(target.leaf_guard, leaf_arguments));
  }
  auto* branch = builder_.CreateCondBr(builder_.CreateNot(ready), observable, leaf,
                                       llvm::MDBuilder(context).createUnlikelyBranchWeights());
  if (target.leaf_guard == nullptr && !target.budgeted_recursion) {
    // 有界叶调用沿用循环的轮询预算；异常分支仍建立完整调用链。
    branch->setMetadata(kNativeBoundedPollMetadata, llvm::MDNode::get(context, {}));
  }
  builder_.SetInsertPoint(leaf);
  if (target.budgeted_recursion) {
    leaf_arguments.insert(leaf_arguments.begin() + layout.budget,
                          builder_.getInt32(kRecursiveLeafDepth));
  }
  auto* computed = builder_.CreateCall(target.leaf, leaf_arguments);
  builder_.CreateCondBr(
      builder_.CreateICmpEQ(computed, Status(builder_, ExecutionStatus::Returned)), returned,
      failed, llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(failed);
  if (target.budgeted_recursion) {
    // 只重放无状态的纯计算；预算耗尽和算术失败都由完整入口建立真实错误调用链。
    builder_.CreateBr(observable);
  } else {
    PrepareObservableCall();
    auto* position = builder_.CreateLoad(builder_.getInt64Ty(), result);
    auto* materialized = EmitLeafFailure({
        .status = computed,
        .position = position,
        .roots = llvm::ConstantPointerNull::get(builder_.getPtrTy()),
        .count = 0,
    });
    Fail(materialized);
  }
  builder_.SetInsertPoint(returned);
  StoreCallResult(instruction.dst, callee.return_type, result);
  builder_.CreateBr(merge);
  builder_.SetInsertPoint(observable);
  PrepareObservableCall();
  auto* called = builder_.CreateCall(target.body, arguments);
  CheckStatus(called);
  StoreCallResult(instruction.dst, callee.return_type, result);
  PollAfterRuntimeCall();
  builder_.CreateBr(merge);
  builder_.SetInsertPoint(merge);
}

void FunctionEmitter::EmitNamedBaselineCall(const ir::Instruction& instruction,
                                            CallableTarget selected) {
  const auto& callee = program_.functions.at(selected.function);
  llvm::Value* receiver_place = nullptr;
  for (std::size_t index = 0; index < callee.parameter_count; ++index) {
    const auto reg = static_cast<ir::RegisterId>(instruction.lhs + index);
    const auto& type = callee.value_types.at(index);
    if (index == 0 && callee.struct_receiver) {
      receiver_place = StructCallReceiver(instruction, reg, type);
      Pack(llvm::Constant::getNullValue(NativeRecordType(module_.getContext())), index);
      continue;
    }
    auto* value = ConvertValue(Load(reg), source_.value_types.at(reg), type);
    Pack(EncodeChecked(value, type), index);
  }
  llvm::Value* arguments = arguments_;
  if (receiver_place != nullptr) {
    // 根区保持普通值；内部位置描述符只进入不被 GC 扫描的同步实参副本。
    Builder allocation(&body_->getEntryBlock(), body_->getEntryBlock().begin());
    auto* records = NativeRecordType(module_.getContext());
    arguments = allocation.CreateAlloca(llvm::ArrayType::get(records, callee.parameter_count));
    for (std::size_t index = 1; index < callee.parameter_count; ++index) {
      builder_.CreateStore(
          builder_.CreateLoad(records, builder_.CreateConstGEP1_64(records, arguments_, index)),
          builder_.CreateConstGEP1_64(records, arguments, index));
    }
    auto* encoded = llvm::Constant::getNullValue(records);
    llvm::Value* receiver = builder_.CreateInsertValue(
        encoded, builder_.CreatePtrToInt(receiver_place, builder_.getInt64Ty()), 0);
    receiver = builder_.CreateInsertValue(
        receiver,
        builder_.getInt8(static_cast<std::uint8_t>(runtime::NativeValueKind::StructPlace)), 1);
    builder_.CreateStore(receiver, arguments);
  }
  auto helper = module_.getOrInsertFunction(
      runtime::kNativeCallDeclarationSymbol, builder_.getInt8Ty(), builder_.getPtrTy(),
      builder_.getInt64Ty(), builder_.getPtrTy(), builder_.getInt64Ty(), builder_.getPtrTy());
  CompleteCall(builder_.CreateCall(helper,
                                   {
                                       body_->getArg(0),
                                       builder_.getInt64(callee.identity.declaration.value),
                                       arguments,
                                       builder_.getInt64(callee.parameter_count),
                                       result_,
                                   }),
               instruction.dst);
}

void FunctionEmitter::EmitCall(const ir::Instruction& instruction, CallableTarget selected) {
  if (kind_ == NativeEntryKind::HostLeaf) {
    EmitHostLeafCall(instruction, selected);
    return;
  }
  const auto& callee = program_.functions.at(selected.function);
  const auto& target = generated_[selected.function];
  if (kind_ == NativeEntryKind::Leaf && target.budgeted_recursion &&
      selected.function == function_index_) {
    auto arguments = CallArguments(instruction, selected, std::nullopt);
    const LeafEntryLayout layout(source_.parameter_count);
    auto* budget = body_->getArg(layout.budget);
    arguments.insert(arguments.begin() + layout.depth, entry_depth_);
    arguments.insert(arguments.begin() + layout.budget,
                     builder_.CreateNUWSub(budget, builder_.getInt32(1)));
    CheckStatus(builder_.CreateCall(target.leaf, arguments));
    StoreCallResult(instruction.dst, callee.return_type, result_);
    return;
  }
  // 即使被调用方使用延迟叶入口，调用者也必须占用深度预算并保留失败时的调用链。
  PrepareTraceFrame();
  auto* intrinsic_done = EmitIntrinsicCall(instruction, selected.function);
  if (tier_ == NativeCodeTier::Baseline && IsNamedBaselineCall(callee, program_.native_mode)) {
    EmitNamedBaselineCall(instruction, selected);
    return;
  }
  const auto selection = SelectCallEntry(selected, instruction_index_);
  auto arguments = CallArguments(instruction, selected, selection.borrowed);
  const auto call_entry = selection.entry;
  const auto leaf_call = call_entry == CallEntry::Leaf;
  auto* result = leaf_call || call_entry == CallEntry::ScalarField ? leaf_result_ : result_;
  arguments.back() = result;
  auto* import_done = EmitInlineImportCall(instruction, selected, arguments);
  llvm::Value* status = nullptr;
  if (call_entry == CallEntry::Slot) {
    // 槽属于本次发布的不可变映像；复用的机器码从执行实例读取新目标。
    // 旧帧在安全点恢复到基线后按声明调用，因此不借此进入旧快照的命名目标。
    using runtime::NativeFunctionDescription;
    using runtime::NativeImage;
    auto* functions = builder_.CreateLoad(builder_.getPtrTy(),
                                          ByteAddress(image_, offsetof(NativeImage, functions)));
    const auto offset = (sizeof(NativeFunctionDescription) * selected.function) +
                        offsetof(NativeFunctionDescription, typed_entry);
    auto* entry = builder_.CreateLoad(builder_.getPtrTy(), ByteAddress(functions, offset),
                                      "call.typed.entry");
    status = builder_.CreateCall(target.body->getFunctionType(), entry, arguments);
  } else if (leaf_call) {
    EmitLeafCall(instruction, selected, arguments);
  } else if (call_entry == CallEntry::ScalarField) {
    status = EmitScalarFieldCall(selected, arguments);
  } else {
    status = builder_.CreateCall(call_entry == CallEntry::Borrowed ? target.borrowed : target.body,
                                 arguments);
  }
  if (selection.borrowed) {
    // 被调用方可能在抛错前修改引用字段；错误封套分配前刷新调用者根。
    StoreRoots(instruction_index_);
  }
  if (!leaf_call) {
    CheckStatus(status);
    StoreCallResult(instruction.dst, callee.return_type, result);
  }
  CacheInitializedGlobals(selected.function);
  if (import_done != nullptr) {
    builder_.CreateBr(import_done);
    builder_.SetInsertPoint(import_done);
  }
  if (intrinsic_done != nullptr) {
    PollAfterRuntimeCall();
    builder_.CreateBr(intrinsic_done);
    builder_.SetInsertPoint(intrinsic_done);
  }
}

}  // namespace vela::compiler::native
