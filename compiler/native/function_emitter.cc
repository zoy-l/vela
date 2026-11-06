#include "compiler/native/function_emitter.h"
#include "base/builtin.h"
#include "base/native_program_mode.h"
#include "compiler/native/array_analysis.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/field_access.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/native_frame_plan.h"
#include "compiler/native/native_roots.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_symbols.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/AtomicOrdering.h>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_deoptimization.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {
bool FunctionEmitter::HasInlineRuntimePath(std::size_t index) const {
  const auto& instruction = source_.instructions.at(index);
  const auto target = CallTarget(index);
  return (instruction.opcode == Opcode::GetField && instruction.field_declaration.has_value()) ||
         (instruction.opcode == Opcode::BuiltinCall &&
          instruction.index == static_cast<std::uint32_t>(Builtin::ArrayPush)) ||
         instruction.opcode == Opcode::GetGlobal ||
         (target && (generated_[target->function].leaf != nullptr ||
                     generated_[target->function].square_root));
}

llvm::Value* FunctionEmitter::Load(ir::RegisterId reg) {
  const auto& type = source_.value_types.at(reg);
  if (IsRecovering()) {
    auto* value = builder_.CreateLoad(NativeRecordType(module_.getContext()), slots_.at(reg));
    return DecodeValue(builder_, value, type);
  }
  return builder_.CreateLoad(ValueType(module_.getContext(), type), slots_.at(reg));
}

llvm::Value* FunctionEmitter::Payload(ir::RegisterId reg) {
  auto* value = Load(reg);
  return PayloadOf(builder_, value, source_.value_types.at(reg));
}

llvm::Value* FunctionEmitter::Reference(ir::RegisterId reg) {
  return DecodeValue(builder_, Encode(reg), {.kind = Kind::Class, .name = {}});
}

void FunctionEmitter::Store(ir::RegisterId reg, llvm::Value* value, const SourceType& type) {
  const auto& target = source_.value_types.at(reg);
  auto* converted = Convert(builder_, value, type, target);
  builder_.CreateStore(IsRecovering() ? EncodeValue(builder_, converted, target) : converted,
                       slots_.at(reg));
}

void FunctionEmitter::Finish(llvm::Value* status) {
  status_->addIncoming(status, builder_.GetInsertBlock());
  builder_.CreateBr(cleanup_);
}

llvm::SwitchInst* FunctionEmitter::ResumeDispatch(llvm::Value* instruction) {
  auto* invalid = llvm::BasicBlock::Create(module_.getContext(), "invalid.resume", body_);
  auto* dispatch = builder_.CreateSwitch(instruction, invalid);
  builder_.SetInsertPoint(invalid);
  Finish(Status(builder_, ExecutionStatus::InvalidEntry));
  return dispatch;
}

void FunctionEmitter::SetPosition() {
  PrepareTraceFrame();
  const auto position = origins_.empty() ? 0 : origins_[instruction_index_];
  builder_.CreateStore(builder_.getInt32(static_cast<std::uint32_t>(position)), instruction_slot_);
}

void FunctionEmitter::PrepareRuntimeCall() {
  if (leaf_entry_) {
    return;
  }
  const auto target = CallTarget(instruction_index_);
  if (target && generated_[target->function].leaf != nullptr) {
    // 有界叶调用不轮询、不分配。失败状态返回调用者后才建立根和逻辑异常帧。
    return;
  }
  PrepareObservableCall();
}

void FunctionEmitter::PrepareObservableCall() {
  SetPosition();
  StoreRoots(instruction_index_);
  PrepareCallCheckpoint();
}

void FunctionEmitter::PrepareCallCheckpoint() {
  // 调用期间的安全点只读写 tier、phase 和物化状态；恢复只发生在本帧此后的安全点，
  // 那里会重新保存规范值。只有虚拟对象可能在被调用方安全点按此处的值物化。
  if (virtual_state_ != nullptr) {
    StoreCheckpoint();
  } else if (checkpoint_ != nullptr) {
    PrepareCheckpoint();
  }
  StoreDebugValues();
  if (checkpoint_ != nullptr) {
    builder_.CreateStore(
        builder_.getInt8(static_cast<std::uint8_t>(runtime::NativeCheckpointPhase::AwaitingResult)),
        ByteAddress(checkpoint_, offsetof(runtime::NativeCheckpoint, phase)));
  }
}

std::uint64_t FunctionEmitter::TracePosition() const {
  return runtime::EncodeNativeTracePosition({
      .function = static_cast<std::uint32_t>(function_index_),
      .instruction = static_cast<std::uint32_t>(origins_[instruction_index_]),
  });
}

llvm::Value* FunctionEmitter::ControlState() {
  auto* state = builder_.CreateLoad(
      builder_.getPtrTy(),
      ByteAddress(body_->getArg(0), runtime::NativeMemoryLayout::kExecutionControlState));
  MarkInvariant(*state);
  return state;
}

llvm::Value* FunctionEmitter::LoadSafepointRequests() {
  auto* requests_address = builder_.CreateLoad(
      builder_.getPtrTy(),
      ByteAddress(ControlState(), offsetof(runtime::NativeControlState, requests)));
  // 请求字属于 Isolate，地址在整个执行期间固定；请求值仍必须在每个安全点原子读取。
  MarkInvariant(*requests_address);
  auto* requests = builder_.CreateLoad(builder_.getInt8Ty(), requests_address);
  // 零值分支不读取跨线程发布的数据；慢路径 ProcessSafepoint 用 acquire 读取并消费请求。
  // 探测本身只需原子性，不能让 acquire 阻断无请求执行片段内的字段和数组优化。
  requests->setAtomic(llvm::AtomicOrdering::Monotonic);
  return requests;
}

void FunctionEmitter::Poll() {
  auto& context = module_.getContext();
  auto* requests = LoadSafepointRequests();
  llvm::Value* required = builder_.CreateICmpNE(requests, builder_.getInt8(0));
  if (checkpoint_ != nullptr) {
    // 被调用方可能已消耗发布请求并使当前帧失效；返回后仍须在下一安全点恢复。
    auto* tier = builder_.CreateLoad(
        builder_.getInt8Ty(), ByteAddress(checkpoint_, offsetof(runtime::NativeCheckpoint, tier)));
    required = builder_.CreateOr(
        required, builder_.CreateICmpEQ(
                      tier, builder_.getInt8(static_cast<std::uint8_t>(NativeCodeTier::Baseline))));
  }
  auto* process = llvm::BasicBlock::Create(context, "safepoint.process", body_);
  auto* ready = llvm::BasicBlock::Create(context, "safepoint.ready", body_);
  auto* branch = builder_.CreateCondBr(required, process, ready,
                                       llvm::MDBuilder(context).createUnlikelyBranchWeights());
  if (roots_.bounded_polls) {
    branch->setMetadata(kNativeBoundedPollMetadata, llvm::MDNode::get(context, {}));
  }
  builder_.SetInsertPoint(process);
  SetPosition();
  if (source_.native_symbol.empty()) {
    StoreRoots(instruction_index_);
    StoreCheckpoint();
  }
  StoreDebugValues();
  auto poll = module_.getOrInsertFunction(runtime::kNativeSafepointSymbol, builder_.getInt8Ty(),
                                          builder_.getPtrTy());
  CheckStatus(builder_.CreateCall(poll, {body_->getArg(0)}));
  if (checkpoint_ != nullptr) {
    RecoverIfInvalidated();
  }
  builder_.CreateBr(ready);
  builder_.SetInsertPoint(ready);
}

bool FunctionEmitter::IsBoundaryPollRequired(std::size_t index) const {
  const auto flags = roots_.safepoints.at(index);
  return (flags & kNativeSafepointBoundary) != 0 ||
         ((flags & kNativeSafepointBeforeRuntime) != 0 &&
          ir::AnalyzeEffects(source_.instructions.at(index)).MayCollectOnSuccess() &&
          !HasInlineRuntimePath(index)) ||
         ((flags & kNativeSafepointAfterRuntime) != 0 &&
          ir::AnalyzeEffects(source_.instructions.at(index - 1)).MayCollectOnSuccess() &&
          !HasInlineRuntimePath(index - 1));
}

void FunctionEmitter::PrepareInlineRuntimeCall() {
  if (program_.native_mode == NativeProgramMode::Reloadable &&
      !IsBoundaryPollRequired(instruction_index_)) {
    Poll();
  }
  PrepareRuntimeCall();
}

void FunctionEmitter::PollAfterRuntimeCall() {
  const auto next = instruction_index_ + 1;
  if (program_.native_mode != NativeProgramMode::Reloadable || roots_.safepoints.at(next) == 0 ||
      IsBoundaryPollRequired(next)) {
    return;
  }
  // 结果已经写回；使用下一条指令的活动值和恢复位置，避免发布后重放本次操作。
  const auto previous = instruction_index_;
  instruction_index_ = next;
  Poll();
  instruction_index_ = previous;
}

void FunctionEmitter::Fail(llvm::Value* status) {
  AbortConstructions();
  if (kind_ == NativeEntryKind::Leaf) {
    // 内部叶入口的失败结果复用 ABI 暂存槽保存源位置；此处禁止分配和进入运行时。
    builder_.CreateStore(builder_.getInt64(TracePosition()), Output());
    Finish(status);
    return;
  }
  if (leaf_entry_) {
    const auto& live = roots_.stores.at(instruction_index_);
    for (std::size_t index = 0; index < live.size(); ++index) {
      WriteHeapValue(RootAddress(index), live.at(index), MemoryRegion::Roots);
    }
    auto failure = module_.getOrInsertFunction(
        runtime::kNativeLeafFailureSymbol, builder_.getInt8Ty(), builder_.getPtrTy(),
        builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getPtrTy(), builder_.getInt64Ty());
    auto* roots = live.empty() ? llvm::ConstantPointerNull::get(builder_.getPtrTy()) : root_slots_;
    Finish(builder_.CreateCall(failure, {
                                            body_->getArg(0),
                                            builder_.getInt64(TracePosition()),
                                            status,
                                            roots,
                                            builder_.getInt64(live.size()),
                                        }));
    return;
  }
  SetPosition();
  if (!source_.instructions.empty()) {
    // 失败封套可以分配；条件式运行时调用的成功分支可能尚未登记当前活动值。
    StoreRoots(instruction_index_);
  }
  auto failure = module_.getOrInsertFunction(runtime::kNativeFailureSymbol, builder_.getInt8Ty(),
                                             builder_.getPtrTy(), builder_.getInt8Ty());
  auto* propagated = builder_.CreateCall(failure, {body_->getArg(0), status});
  const auto handler = source_.instructions.empty()
                           ? std::optional<std::uint32_t>()
                           : source_.instructions.at(instruction_index_).handler;
  if (handler) {
    auto pending = module_.getOrInsertFunction(runtime::kNativeHasExceptionSymbol,
                                               builder_.getInt8Ty(), builder_.getPtrTy());
    auto* has_exception = builder_.CreateCall(pending, {body_->getArg(0)});
    auto* unhandled = llvm::BasicBlock::Create(module_.getContext(), "unhandled", body_);
    // handler 的下一安全点会登记其活动值；此处到 handler 之间不再分配。
    // 批量清空根区，避免每个失败点展开整个函数的根槽，生成平方规模的 LLVM IR。
    if (roots_.count != 0) {
      static_assert(static_cast<std::uint8_t>(runtime::ValueKind::Null) == 0);
      builder_.CreateMemSet(root_slots_, builder_.getInt8(0), roots_.count * sizeof(runtime::Value),
                            llvm::MaybeAlign(alignof(runtime::Value)));
    }
    const auto target = source_.handlers.at(*handler).target;
    builder_.CreateCondBr(builder_.CreateICmpNE(has_exception, builder_.getInt8(0)),
                          blocks_.at(target), unhandled);
    builder_.SetInsertPoint(unhandled);
  }
  if (source_.is_async) {
    EmitAsyncFailure(propagated);
  } else {
    Finish(propagated);
  }
}

void FunctionEmitter::Check(llvm::Value* condition, ExecutionStatus failure) {
  auto* failed = llvm::BasicBlock::Create(module_.getContext(), "failed", body_);
  auto* next = llvm::BasicBlock::Create(module_.getContext(), "checked", body_);
  builder_.CreateCondBr(condition, next, failed,
                        llvm::MDBuilder(module_.getContext()).createLikelyBranchWeights());
  builder_.SetInsertPoint(failed);
  Fail(Status(builder_, failure));
  builder_.SetInsertPoint(next);
}

void FunctionEmitter::CheckStatus(llvm::Value* status) {
  auto* failed = llvm::BasicBlock::Create(module_.getContext(), "call.failed", body_);
  auto* next = llvm::BasicBlock::Create(module_.getContext(), "call.returned", body_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(status, Status(builder_, ExecutionStatus::Returned)),
                        next, failed,
                        llvm::MDBuilder(module_.getContext()).createLikelyBranchWeights());
  builder_.SetInsertPoint(failed);
  if (program_.native_mode == NativeProgramMode::Reloadable) {
    auto* stopped = llvm::BasicBlock::Create(module_.getContext(), "execution.stopped", body_);
    auto* propagate = llvm::BasicBlock::Create(module_.getContext(), "execution.failure", body_);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(status, Status(builder_, ExecutionStatus::ExecutionStopped)), stopped,
        propagate);
    builder_.SetInsertPoint(stopped);
    Finish(status);
    builder_.SetInsertPoint(propagate);
  }
  Fail(status);
  builder_.SetInsertPoint(next);
}

void FunctionEmitter::EmitConstant(const ir::Instruction& instruction) {
  switch (instruction.opcode) {
    case Opcode::LoadNull:
      Store(instruction.dst, builder_.getFalse(),
            {.kind = Kind::Null, .name = frontend::kNullTypeName});
      break;
    case Opcode::LoadBool:
      Store(instruction.dst, builder_.getInt1(instruction.immediate != 0),
            {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
      break;
    case Opcode::LoadInt:
      Store(instruction.dst, builder_.getInt64(std::bit_cast<std::uint64_t>(instruction.immediate)),
            {.kind = Kind::Int, .name = frontend::kIntTypeName});
      break;
    case Opcode::LoadDouble:
      Store(instruction.dst,
            llvm::ConstantFP::get(builder_.getDoubleTy(), source_.doubles.at(instruction.index)),
            {.kind = Kind::Double, .name = frontend::kDoubleTypeName});
      break;
    default:
      std::unreachable();
  }
}

std::expected<void, std::string> FunctionEmitter::EmitInstruction(
    const ir::Instruction& instruction) {
  switch (instruction.opcode) {
    case Opcode::LoadNull:
    case Opcode::LoadBool:
    case Opcode::LoadInt:
    case Opcode::LoadDouble:
      EmitConstant(instruction);
      break;
    case Opcode::Move:
      Store(instruction.dst, Load(instruction.lhs), source_.value_types.at(instruction.lhs));
      break;
    case Opcode::AddInt:
    case Opcode::SubInt:
    case Opcode::MulInt:
    case Opcode::DivInt:
    case Opcode::ModInt:
    case Opcode::NegInt:
    case Opcode::BitNotInt:
    case Opcode::BitAndInt:
    case Opcode::BitOrInt:
    case Opcode::BitXorInt:
    case Opcode::ShiftLeftInt:
    case Opcode::ShiftRightInt:
    case Opcode::ShiftUnsignedRightInt:
      EmitInteger(instruction);
      break;
    case Opcode::AddDouble:
    case Opcode::SubDouble:
    case Opcode::MulDouble:
    case Opcode::DivDouble:
    case Opcode::NegDouble:
      EmitDouble(instruction);
      break;
    case Opcode::EqualInt:
    case Opcode::NotEqualInt:
    case Opcode::LessInt:
    case Opcode::LessEqualInt:
    case Opcode::GreaterInt:
    case Opcode::GreaterEqualInt:
    case Opcode::EqualBool:
    case Opcode::NotEqualBool:
    case Opcode::EqualDouble:
    case Opcode::NotEqualDouble:
    case Opcode::LessDouble:
    case Opcode::LessEqualDouble:
    case Opcode::GreaterDouble:
    case Opcode::GreaterEqualDouble:
      EmitComparison(instruction);
      break;
    case Opcode::EqualValue:
    case Opcode::NotEqualValue:
      EmitValueEquality(instruction);
      break;
    case Opcode::NotBool:
      Store(instruction.dst, builder_.CreateNot(Payload(instruction.lhs)),
            {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
      break;
    case Opcode::Jump:
      builder_.CreateBr(blocks_.at(instruction.index));
      break;
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull: {
      llvm::Value* condition = nullptr;
      if (instruction.opcode == Opcode::JumpIfFalse) {
        condition = builder_.CreateNot(Payload(instruction.lhs));
      } else {
        const auto& type = source_.value_types.at(instruction.lhs);
        condition = builder_.CreateNot(IsPresent(builder_, Load(instruction.lhs), type));
      }
      const auto position = static_cast<std::size_t>(&instruction - source_.instructions.data());
      auto* next = blocks_.at(position + 1);
      ClearRoots(position);
      builder_.CreateCondBr(condition, blocks_.at(instruction.index), next);
      break;
    }
    case Opcode::Call:
      EmitCall(instruction, {.function = instruction.index});
      break;
    case Opcode::Await:
      EmitAwait(instruction);
      break;
    case Opcode::NewFuture:
    case Opcode::SleepFuture:
    case Opcode::AsyncCall:
      EmitAsyncInstruction(instruction);
      break;
    case Opcode::Return: {
      if (source_.is_async) {
        EmitAsyncReturn(instruction);
        break;
      }
      if (source_.return_type.kind != Kind::Void) {
        auto* result = Convert(builder_, Load(instruction.dst),
                               source_.value_types.at(instruction.dst), source_.return_type);
        builder_.CreateStore(result, Output());
      }
      Finish(Status(builder_, ExecutionStatus::Returned));
      break;
    }
    default:
      return EmitReference(instruction);
  }
  return {};
}

void FunctionEmitter::PrepareFrame() {
  // PromotePass 只处理入口块中的 alloca；可重载映像的有效性分支不能把局部槽
  // 放到后继块，否则前置范围分析看到的仍是内存寄存器文件。
  Builder allocation(&body_->getEntryBlock(), body_->getEntryBlock().begin());
  slots_.resize(source_.register_count);
  if (std::ranges::any_of(source_.instructions, [](const auto& instruction) {
        return instruction.construction.has_value();
      })) {
    construction_depth_ = allocation.CreateAlloca(builder_.getInt32Ty());
    builder_.CreateStore(builder_.getInt32(0), construction_depth_);
  }
  const auto recovery = HasRecovery() ? NativeFrameRecovery::Required : NativeFrameRecovery::None;
  const auto frame = PlanNativeFrame(module_, program_, escape_, roots_, temporary_count_, recovery,
                                     kind_, generated_, direct_calls_);
  frame_type_ = frame.type;
  frame_locals_ = frame.local_fields;
  temporary_field_ = frame.temporary_field;
  leaf_result_field_ = frame.leaf_result_field;
  const std::vector<llvm::Type*> fields(frame_type_->element_begin(), frame_type_->element_end());
  storage_bytes_ = module_.getDataLayout().getTypeAllocSize(frame_type_);
  frame_bytes_ = frame.bytes;
  const auto on_stack = frame_bytes_ <= kMaximumNativeStackBytes;
  if (!IsRecovering()) {
    // 局部值的地址不传给 helper；独立 alloca 可提升为 SSA。将它们与 ABI 暂存区
    // 放在同一结构中会让暂存区的地址逃逸阻止提升，导致纯算术循环持续读写栈。
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      const auto& type = source_.value_types.at(index);
      if (type.kind != Kind::Invalid && (on_stack || !frame_locals_.at(index))) {
        slots_.at(index) = allocation.CreateAlloca(ValueType(module_.getContext(), type));
      }
    }
  }
  if (!IsRecovering() && on_stack) {
    arguments_ = allocation.CreateAlloca(fields.at(temporary_field_));
    result_ = allocation.CreateAlloca(fields.at(temporary_field_ + 1));
    if (leaf_result_field_) {
      // 纯计算结果不与会传入运行时的 ABI 暂存槽共享地址，内联后可提升为 SSA。
      leaf_result_ =
          allocation.CreateAlloca(fields.at(*leaf_result_field_), nullptr, "leaf.result");
    }
    if (roots_.debug_values) {
      auto* values = allocation.CreateAlloca(fields.back());
      values->setAlignment(llvm::Align(alignof(runtime::NativeDebugValue)));
      debug_values_ = values;
    }
    if (HasRecovery()) {
      auto* checkpoint = allocation.CreateAlloca(fields.at(temporary_field_ + 2));
      checkpoint->setAlignment(llvm::Align(alignof(runtime::NativeCheckpoint)));
      checkpoint_ = checkpoint;
      checkpoint_values_ = allocation.CreateAlloca(fields.at(temporary_field_ + 3));
      recovery_storage_ = allocation.CreateAlloca(fields.at(temporary_field_ + 4));
      if (!escape_.checkpoints.empty()) {
        virtual_references_ =
            allocation.CreateAlloca(fields.at(temporary_field_ + kVirtualReferenceFieldIndex));
        auto* state =
            allocation.CreateAlloca(fields.at(temporary_field_ + kVirtualStateFieldIndex));
        state->setAlignment(llvm::Align(alignof(runtime::NativeVirtualState)));
        virtual_state_ = state;
      }
    }
  }
}

bool FunctionEmitter::CanUseLeafEntry() const {
  if (roots_.debug_values ||
      (program_.native_mode == NativeProgramMode::Reloadable && kind_ != NativeEntryKind::Leaf) ||
      source_.is_async || !source_.native_symbol.empty() || !source_.handlers.empty() ||
      frame_bytes_ > kMaximumNativeStackBytes || !IsScalar(source_.return_type)) {
    return false;
  }
  return std::ranges::all_of(source_.instructions, [&](const ir::Instruction& instruction) {
    // 标量 return 不装箱；字段和数组读写可保留副作用，但不能进入用户代码或分配。
    if (instruction.opcode == Opcode::Return) {
      return true;
    }
    if (instruction.opcode == Opcode::GetGlobal) {
      // 惰性初始化器可能 GC；仅在没有活动引用时沿用延迟逻辑帧入口。
      return roots_.count == 0;
    }
    const auto effects = ir::AnalyzeEffects(instruction);
    return !effects.MayCollectOnSuccess();
  });
}

void FunctionEmitter::BindFrameStorage() {
  if (IsRecovering()) {
    frame_storage_ = builder_.CreateLoad(
        builder_.getPtrTy(),
        ByteAddress(body_->getArg(1), offsetof(runtime::NativeCheckpoint, storage)));
    root_slots_ = builder_.CreateLoad(
        builder_.getPtrTy(),
        ByteAddress(body_->getArg(1), offsetof(runtime::NativeCheckpoint, roots)));
    auto* values = builder_.CreateLoad(
        builder_.getPtrTy(),
        ByteAddress(body_->getArg(1), offsetof(runtime::NativeCheckpoint, values)));
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      slots_.at(index) = ByteAddress(values, index * sizeof(runtime::NativeValue));
    }
    arguments_ = builder_.CreateStructGEP(frame_type_, frame_storage_, 0);
    result_ = builder_.CreateStructGEP(frame_type_, frame_storage_, 1);
    if (roots_.debug_values) {
      debug_values_ = builder_.CreateStructGEP(frame_type_, frame_storage_, 2);
    }
    return;
  }
  if (frame_bytes_ <= kMaximumNativeStackBytes) {
    return;
  }
  for (std::size_t index = 0; index < slots_.size(); ++index) {
    if (const auto field = frame_locals_.at(index)) {
      slots_.at(index) = builder_.CreateStructGEP(frame_type_, frame_storage_, *field);
    }
  }
  const auto scratch = temporary_field_;
  arguments_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch);
  result_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + 1);
  if (leaf_result_field_) {
    leaf_result_ = builder_.CreateStructGEP(frame_type_, frame_storage_, *leaf_result_field_);
  }
  if (roots_.debug_values) {
    debug_values_ =
        builder_.CreateStructGEP(frame_type_, frame_storage_, frame_type_->getNumElements() - 1);
  }
  if (HasRecovery()) {
    checkpoint_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + 2);
    checkpoint_values_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + 3);
    recovery_storage_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + 4);
    if (!escape_.checkpoints.empty()) {
      virtual_references_ = builder_.CreateStructGEP(frame_type_, frame_storage_,
                                                     scratch + kVirtualReferenceFieldIndex);
      virtual_state_ =
          builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + kVirtualStateFieldIndex);
    }
  }
}

std::expected<void, std::string> FunctionEmitter::EmitInstructionBlock(std::size_t index) {
  const auto& instruction = source_.instructions.at(index);
  instruction_index_ = index;
  builder_.SetInsertPoint(blocks_.at(index));
  const auto effects = ir::AnalyzeEffects(instruction);
  const auto safepoint = IsBoundaryPollRequired(index);
  if (safepoint && kind_ != NativeEntryKind::Leaf) {
    Poll();
  }
  if (effects.MayCollectOnSuccess() && !HasInlineRuntimePath(index)) {
    PrepareRuntimeCall();
  }
  if (instruction.construction) {
    BeginConstruction();
  }
  const auto emitted = EmitInstruction(instruction);
  if (!emitted) {
    return emitted;
  }
  if (instruction.completes_construction) {
    EndConstruction();
  }
  if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
    ClearRoots(index);
    builder_.CreateBr(blocks_.at(index + 1));
  }
  return {};
}

void FunctionEmitter::BindArguments() {
  if (kind_ == NativeEntryKind::Initial || kind_ == NativeEntryKind::Borrowed ||
      kind_ == NativeEntryKind::Leaf) {
    for (std::size_t index = 0; index < source_.parameter_count; ++index) {
      auto* value =
          kind_ == NativeEntryKind::Borrowed && index == 0
              ? llvm::ConstantPointerNull::get(builder_.getPtrTy())
              : static_cast<llvm::Value*>(body_->getArg(static_cast<unsigned>(index + 1)));
      builder_.CreateStore(value, slots_.at(index));
    }
  }
  if (kind_ == NativeEntryKind::Borrowed) {
    for (std::size_t index = 0; index < escape_.borrowed_fields.size(); ++index) {
      auto* address = builder_.CreateConstGEP1_64(builder_.getPtrTy(), body_->getArg(1), index);
      slots_.at(escape_.borrowed_fields.at(index)) =
          builder_.CreateLoad(builder_.getPtrTy(), address);
    }
  }
}

void FunctionEmitter::InitializeRecords() {
  records_.resize(escape_.allocations.size());
  for (std::size_t index = 0; index < escape_.allocations.size(); ++index) {
    const auto& site = escape_.allocations.at(index);
    if (!site.stack_record) {
      continue;
    }
    auto* record =
        builder_.CreateAlloca(llvm::ArrayType::get(builder_.getPtrTy(), site.fields.size()));
    records_.at(index) = record;
    for (std::size_t field = 0; field < site.fields.size(); ++field) {
      builder_.CreateStore(slots_.at(site.fields.at(field)),
                           builder_.CreateConstGEP1_64(builder_.getPtrTy(), record, field));
    }
  }
}

std::expected<void, std::string> FunctionEmitter::Emit() {
  auto& context = module_.getContext();
  auto* entry = llvm::BasicBlock::Create(context, "entry", body_);
  builder_.SetInsertPoint(entry);
  if (kind_ == NativeEntryKind::Resume) {
    EmitExecutionBoundary(builder_);
  }
  if (program_.native_mode == NativeProgramMode::Reloadable) {
    image_ = builder_.CreateLoad(builder_.getPtrTy(),
                                 ByteAddress(LoadProgramData(builder_, body_->getArg(0)),
                                             runtime::NativeMemoryLayout::kProgramImage));
  }
  GuardResume();
  PrepareFrame();
  leaf_entry_ = CanUseLeafEntry();
  EmitFrameEntry();
  BindFrameStorage();
  InitializeCheckpoint();
  CacheImports();
  CacheGlobals();
  CacheFields();
  BindArguments();
  InitializeRecords();
  if (!source_.native_symbol.empty()) {
    EmitImport();
    return {};
  }
  for (std::size_t index = 0; index <= source_.instructions.size(); ++index) {
    blocks_.push_back(llvm::BasicBlock::Create(context, "instruction", body_));
  }
  if (IsRecovering()) {
    EmitRecoveryDispatch();
  } else if (kind_ == NativeEntryKind::Resume) {
    EmitResumeDispatch();
  } else {
    builder_.CreateBr(blocks_.front());
  }
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    if (const auto emitted = EmitInstructionBlock(index); !emitted) {
      return emitted;
    }
  }
  builder_.SetInsertPoint(blocks_.back());
  Finish(Status(builder_, ExecutionStatus::InvalidEntry));
  return {};
}

namespace {

NativeRootPlan EscapeRoots(const ir::Program& program,
                           std::size_t function_index,
                           const EscapePlan& escape,
                           NativeDebugging debugging) {
  const auto& function = escape.function.get();
  std::vector<std::vector<ir::RegisterId>> required;
  std::vector<std::uint8_t> safepoints;
  NativeRecoveryRequirements requirements;
  if (!escape.checkpoints.empty()) {
    const auto& canonical = program.functions.at(function_index);
    const auto canonical_roots =
        PlanNativeRoots(canonical, program.functions, program.native_mode, debugging);
    const auto temporary = PlanTemporaryCount(canonical, program, canonical_roots);
    required.resize(function.instructions.size());
    safepoints.resize(function.instructions.size());
    for (std::size_t index = 0; index < escape.origins.size(); ++index) {
      const auto origin = escape.origins.at(index);
      required.at(index) = escape.checkpoints.at(origin).required;
      if (index == 0 || origin != escape.origins.at(index - 1)) {
        safepoints.at(index) = canonical_roots.safepoints.at(origin);
      }
    }
    requirements = {
        .values = required,
        .safepoints = safepoints,
        .minimum_root_count = canonical_roots.count,
        .minimum_temporary_count = temporary,
    };
  }
  if (std::ranges::any_of(escape.borrowed_calls,
                          [](const auto& call) { return call.has_value(); })) {
    required.resize(function.instructions.size());
    for (std::size_t index = 0; index < escape.borrowed_calls.size(); ++index) {
      if (const auto object = escape.borrowed_calls.at(index)) {
        required.at(index) = escape.allocations.at(*object).fields;
      }
    }
    requirements.values = required;
  }
  return PlanNativeRoots(function, program.functions, program.native_mode, debugging, requirements);
}

FunctionEmissionFacts PlanEmissionFacts(const ir::Program& program,
                                        const FieldTargets& field_targets,
                                        std::size_t function_index,
                                        const EscapePlan& escape,
                                        const NativeRootPlan& roots,
                                        NativeCodeTier tier) {
  FunctionEmissionFacts facts{
      .field_targets = &field_targets,
      .temporary_count = PlanTemporaryCount(escape.function.get(), program, roots),
      .field_accesses = {},
      .direct_calls = {},
      .array_element_bits = {},
  };
  const auto& canonical = program.functions.at(function_index);
  if (roots.bounded_polls) {
    // 规范 IR 保留经对象字段、闭包和借用调用逃逸的真实用途。
    facts.array_element_bits = FindPrivateArrayElementBits(canonical, program);
  }
  if (tier == NativeCodeTier::Optimized) {
    facts.direct_calls = FindDirectCalls(escape.function.get(), program);
  }
  const auto& instructions = canonical.instructions;
  facts.field_accesses.resize(instructions.size());
  absl::flat_hash_map<DeclarationId, std::uint32_t> field_slots;
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    if (const auto declaration = instructions.at(index).field_declaration) {
      const auto slot =
          field_slots.try_emplace(*declaration, static_cast<std::uint32_t>(field_slots.size()))
              .first;
      facts.field_accesses.at(index) = slot->second;
    }
  }
  return facts;
}

void AccumulateMetrics(NativeFunctionMetrics& metrics,
                       const EscapePlan& escape,
                       const NativeRootPlan& roots) {
  for (const auto& allocation : escape.allocations) {
    metrics.stack_records += allocation.stack_record ? 1 : 0;
    if (allocation.can_scalar_replace) {
      if (allocation.materialized) {
        ++metrics.sunk_allocations;
      } else {
        ++metrics.eliminated_allocations;
      }
    } else {
      ++metrics.retained_allocations;
    }
  }
  for (const auto& stores : roots.stores) {
    metrics.root_stores += stores.size();
  }
}

}  // namespace

std::expected<NativeFunctionMetrics, std::string> EmitFunction(
    llvm::Module& module,
    const ir::Program& program,
    const NativeCallPlan& calls,
    const FieldTargets& field_targets,
    llvm::Constant* image,
    std::size_t function_index,
    std::span<const GeneratedFunction> generated,
    std::vector<std::size_t>& body_dependencies,
    NativeDebugging debugging,
    NativeCodeTier tier,
    EscapeOptimization optimization,
    std::span<const std::optional<ir::ClassId>> borrowable) {
  const auto escape = PlanEscapes(program, function_index, optimization, tier, borrowable);
  const auto roots = EscapeRoots(program, function_index, escape, debugging);
  const auto& function = generated[function_index];
  const auto frame_recovery =
      function.HasRecovery(tier) ? NativeFrameRecovery::Required : NativeFrameRecovery::None;
  const auto facts = PlanEmissionFacts(program, field_targets, function_index, escape, roots, tier);
  if (std::ranges::any_of(escape.allocations, &AllocationSite::stack_record)) {
    const auto frame =
        PlanNativeFrame(module, program, escape, roots, facts.temporary_count, frame_recovery,
                        NativeEntryKind::Initial, generated, facts.direct_calls);
    if (frame.bytes > kMaximumNativeStackBytes) {
      // 在 LLVM body 发射前完成表示选择；超预算记录使用普通调用所需的堆物化。
      return EmitFunction(module, program, calls, field_targets, image, function_index, generated,
                          body_dependencies, debugging, tier,
                          EscapeOptimization::ScalarReplacement);
    }
  }
  body_dependencies.insert(body_dependencies.end(), escape.body_dependencies.begin(),
                           escape.body_dependencies.end());
  FunctionEmitter initial(module, program, calls, image, function_index, escape, generated, roots,
                          facts, NativeEntryKind::Initial, tier);
  if (const auto emitted = initial.Emit(); !emitted) {
    return std::unexpected(emitted.error());
  }
  NativeFunctionMetrics metrics{.shadow_slots = roots.count, .frame_bytes = initial.frame_bytes()};
  AccumulateMetrics(metrics, escape, roots);
  if (function.leaf != nullptr) {
    FunctionEmitter leaf(module, program, calls, image, function_index, escape, generated, roots,
                         facts, NativeEntryKind::Leaf, NativeCodeTier::Baseline);
    if (const auto emitted = leaf.Emit(); !emitted) {
      return std::unexpected(emitted.error());
    }
    if (function.leaf_guard != nullptr) {
      EmitBoundedLeafGuard(function);
    }
  }
  if (function.resume != nullptr) {
    FunctionEmitter resume(module, program, calls, image, function_index, escape, generated, roots,
                           facts, NativeEntryKind::Resume, tier);
    if (const auto emitted = resume.Emit(); !emitted) {
      return std::unexpected(emitted.error());
    }
    metrics.frame_bytes = std::max(metrics.frame_bytes, resume.frame_bytes());
  }
  if (tier == NativeCodeTier::Baseline && function.recovery != nullptr) {
    FunctionEmitter recovery(module, program, calls, image, function_index, escape, generated,
                             roots, facts, NativeEntryKind::RecoverInitial, tier);
    if (const auto emitted = recovery.Emit(); !emitted) {
      return std::unexpected(emitted.error());
    }
    if (function.resume_recovery != nullptr) {
      FunctionEmitter resume(module, program, calls, image, function_index, escape, generated,
                             roots, facts, NativeEntryKind::RecoverResume, tier);
      if (const auto emitted = resume.Emit(); !emitted) {
        return std::unexpected(emitted.error());
      }
    }
  }
  return metrics;
}

std::expected<NativeFunctionMetrics, std::string> EmitBorrowedFunction(
    llvm::Module& module,
    const ir::Program& program,
    const NativeCallPlan& calls,
    const FieldTargets& field_targets,
    llvm::Constant* image,
    std::size_t function_index,
    const EscapePlan& plan,
    std::span<const GeneratedFunction> generated,
    NativeDebugging debugging) {
  const auto& function = plan.function.get();
  const std::vector<std::vector<ir::RegisterId>> required(function.instructions.size(),
                                                          plan.borrowed_fields);
  const auto roots = PlanNativeRoots(function, program.functions, program.native_mode, debugging,
                                     {.values = required, .safepoints = {}});
  const auto facts = PlanEmissionFacts(program, field_targets, function_index, plan, roots,
                                       NativeCodeTier::Baseline);
  FunctionEmitter borrowed(module, program, calls, image, function_index, plan, generated, roots,
                           facts, NativeEntryKind::Borrowed, NativeCodeTier::Baseline);
  if (const auto emitted = borrowed.Emit(); !emitted) {
    return std::unexpected(emitted.error());
  }
  NativeFunctionMetrics metrics{.shadow_slots = roots.count, .frame_bytes = borrowed.frame_bytes()};
  AccumulateMetrics(metrics, plan, roots);
  return metrics;
}

}  // namespace vela::compiler::native
