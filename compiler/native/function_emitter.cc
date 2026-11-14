#include "compiler/native/function_emitter.h"
#include "base/builtin.h"
#include "base/native_program_mode.h"
#include "base/reflection.h"
#include "base/reflection_operation.h"
#include "compiler/native/array_analysis.h"
#include "compiler/native/array_reduction_optimizer.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/field_access.h"
#include "compiler/native/global_analysis.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/memory_regions.h"
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

#include "base/callable.h"
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
bool FunctionEmitter::IsLeafCall(std::optional<CallableTarget> target,
                                 std::size_t instruction) const {
  return target && SelectCallEntry(*target, instruction) == CallEntry::Leaf;
}

bool FunctionEmitter::HasInlineRuntimePath(std::size_t index) const {
  const auto& instruction = source_.instructions.at(index);
  const auto target = CallTarget(index);
  if (!reflection_members_.empty()) {
    const auto* member = reflection_members_[index];
    if (member != nullptr && (member->kind == reflection::MemberKind::Field ||
                              generated_[member->target].leaf != nullptr)) {
      return true;
    }
  }
  if (ir::IsBuiltinCall(instruction, Builtin::MapSet) && !map_lookups_.empty() &&
      map_lookups_[index] != nullptr) {
    return true;
  }
  return (instruction.opcode == Opcode::GetField && instruction.field_declaration.has_value()) ||
         ir::IsBuiltinCall(instruction, Builtin::ArrayPush) ||
         (placement_ == NativeCodePlacement::Linked &&
          ir::IsBuiltinCall(instruction, Builtin::MutableStringAppendCodePoint)) ||
         instruction.opcode == Opcode::GetGlobal || IsLeafCall(target, index) ||
         (target && generated_[target->function].square_root);
}

void FunctionEmitter::CacheRuntimeCalls() {
  runtime_calls_.resize(source_.instructions.size());
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    const auto& instruction = source_.instructions[index];
    // 同步 typed return 只转换并写回结果；返回边界的 Poll 独立保留，
    // 不为不存在的运行时调用保存根和 AwaitingResult 检查点。
    const auto synchronous_return = instruction.opcode == Opcode::Return && !source_.is_async;
    runtime_calls_[index] = static_cast<std::uint8_t>(
        !synchronous_return && ir::AnalyzeEffects(instruction).MayCollectOnSuccess() &&
        !HasInlineRuntimePath(index));
  }
}

llvm::Value* FunctionEmitter::Load(ir::RegisterId reg) {
  const auto& type = source_.value_types.at(reg);
  if (IsRecovering()) {
    auto* value = builder_.CreateLoad(NativeRecordType(module_.getContext()), slots_.at(reg));
    return DecodeValue(builder_, value, type);
  }
  return builder_.CreateLoad(ValueType(module_.getContext(), type), slots_.at(reg));
}

bool FunctionEmitter::HasRangeProof(std::span<const std::uint8_t> proofs) const {
  if (proofs.empty()) {
    return false;
  }
  const auto origin = origins_[instruction_index_];
  const auto& canonical = CanonicalInstruction(origin);
  const auto& instruction = source_.instructions[instruction_index_];
  // 逃逸改写可能在同一源位置插入其他操作，不能借用原指令的数值证明。
  return proofs[origin] != 0 && canonical.opcode == instruction.opcode &&
         canonical.dst == instruction.dst && canonical.lhs == instruction.lhs &&
         canonical.rhs == instruction.rhs && canonical.index == instruction.index &&
         canonical.array_type == instruction.array_type;
}

const ir::Instruction& FunctionEmitter::CanonicalInstruction(std::size_t origin) const {
  return program_.functions[function_index_].instructions[origin];
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
  StoreFixedArrayStorage(reg, converted);
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
  StoreMemory(builder_.getInt32(static_cast<std::uint32_t>(position)), instruction_slot_,
              MemoryRegion::Execution);
}

void FunctionEmitter::PrepareRuntimeCall() {
  if (leaf_entry_) {
    return;
  }
  if (IsLeafCall(CallTarget(instruction_index_), instruction_index_)) {
    // 有界叶调用不轮询、不分配。失败状态返回调用者后才建立根和逻辑异常帧。
    return;
  }
  PrepareObservableCall();
}

void FunctionEmitter::PrepareObservableCall() {
  if (host_trace_active_ != nullptr) {
    const auto target = CallTarget(instruction_index_);
    if (!target || !CanInlineImportCall(program_.functions[target->function])) {
      ReleaseBorrowedHostFrame();
    }
  }
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
  if (control_state_ == nullptr) {
    Builder entry(&body_->getEntryBlock(), body_->getEntryBlock().begin());
    auto* address = entry.CreateConstGEP1_64(entry.getInt8Ty(), body_->getArg(0),
                                             runtime::NativeMemoryLayout::kExecutionControlState);
    auto* state = entry.CreateLoad(entry.getPtrTy(), address);
    MarkInvariant(*state);
    control_state_ = state;
  }
  return control_state_;
}

llvm::Value* FunctionEmitter::ProgramData() {
  if (program_data_ == nullptr) {
    Builder entry(&body_->getEntryBlock(), body_->getEntryBlock().begin());
    // 函数入口已绑定所属实例；闭包调用和宿主重入在返回前恢复同一实例。
    // 活动调用保活其程序状态，故本次调用可复用入口借用而不假定执行字段永不写入。
    program_data_ = LoadProgramData(entry, body_->getArg(0));
  }
  return program_data_;
}

llvm::Value* FunctionEmitter::LoadSafepointRequests() {
  auto* requests_address = builder_.CreateLoad(
      builder_.getPtrTy(),
      ByteAddress(ControlState(), offsetof(runtime::NativeControlState, requests)));
  // 请求字属于 Isolate，地址在整个执行期间固定；请求值仍必须在每个安全点原子读取。
  MarkInvariant(*requests_address);
  auto* requests = builder_.CreateLoad(builder_.getInt8Ty(), requests_address);
  requests->setMetadata(
      kNativePollReadyStateMetadata,
      llvm::MDNode::get(module_.getContext(), llvm::ConstantAsMetadata::get(builder_.getInt8(0))));
  // 零值分支不读取跨线程发布的数据；慢路径 ProcessSafepoint 用 acquire 读取并消费请求。
  // 探测本身只需原子性，不能让 acquire 阻断无请求执行片段内的字段和数组优化。
  requests->setAtomic(llvm::AtomicOrdering::Monotonic);
  return requests;
}

llvm::Value* FunctionEmitter::Poll() {
  auto& context = module_.getContext();
  auto* requests = LoadSafepointRequests();
  llvm::Value* required = builder_.CreateICmpNE(requests, builder_.getInt8(0));
  if (checkpoint_ != nullptr) {
    // 被调用方可能已消耗发布请求并使当前帧失效；返回后仍须在下一安全点恢复。
    // 尚未链接的检查点不可能失效，也不读取其未初始化的 tier。
    auto* inspect = llvm::BasicBlock::Create(context, "safepoint.tier", body_);
    auto* checked = llvm::BasicBlock::Create(context, "safepoint.checked", body_);
    auto* unpublished = builder_.GetInsertBlock();
    builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), checkpoint_initialized_),
                          inspect, checked);
    builder_.SetInsertPoint(inspect);
    auto* tier = builder_.CreateLoad(
        builder_.getInt8Ty(), ByteAddress(checkpoint_, offsetof(runtime::NativeCheckpoint, tier)));
    tier->setMetadata(
        kNativePollReadyStateMetadata,
        llvm::MDNode::get(context, llvm::ConstantAsMetadata::get(builder_.getInt8(
                                       static_cast<std::uint8_t>(NativeCodeTier::Optimized)))));
    auto* invalidated = builder_.CreateOr(
        required, builder_.CreateICmpEQ(
                      tier, builder_.getInt8(static_cast<std::uint8_t>(NativeCodeTier::Baseline))));
    builder_.CreateBr(checked);
    builder_.SetInsertPoint(checked);
    auto* needs_poll = builder_.CreatePHI(builder_.getInt1Ty(), 2);
    needs_poll->addIncoming(required, unpublished);
    needs_poll->addIncoming(invalidated, inspect);
    required = needs_poll;
  }
  auto* process = llvm::BasicBlock::Create(context, "safepoint.process", body_);
  auto* ready = llvm::BasicBlock::Create(context, "safepoint.ready", body_);
  auto* branch = builder_.CreateCondBr(required, process, ready,
                                       llvm::MDBuilder(context).createUnlikelyBranchWeights());
  if (roots_.bounded_polls) {
    branch->setMetadata(kNativeBoundedPollMetadata, llvm::MDNode::get(context, {}));
  }
  builder_.SetInsertPoint(process);
  ReleaseBorrowedHostFrame();
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
  return builder_.CreateNot(required);
}

bool FunctionEmitter::IsBoundaryPollRequired(std::size_t index) const {
  const auto flags = roots_.safepoints.at(index);
  return (flags & kNativeSafepointBoundary) != 0 ||
         ((flags & kNativeSafepointBeforeRuntime) != 0 && runtime_calls_[index] != 0) ||
         ((flags & kNativeSafepointAfterRuntime) != 0 && runtime_calls_[index - 1] != 0);
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

void FunctionEmitter::CheckReference(llvm::Value* reference) {
  Check(builder_.CreateIsNotNull(reference), ExecutionStatus::TypeError);
}

llvm::Value* FunctionEmitter::EmitLeafFailure(LeafFailure failure) {
  auto helper = module_.getOrInsertFunction(
      runtime::kNativeLeafFailureSymbol, builder_.getInt8Ty(), builder_.getPtrTy(),
      builder_.getInt64Ty(), builder_.getInt8Ty(), builder_.getPtrTy(), builder_.getInt64Ty());
  return builder_.CreateCall(helper, {
                                         body_->getArg(0),
                                         failure.position,
                                         failure.status,
                                         failure.roots,
                                         builder_.getInt64(failure.count),
                                     });
}

void FunctionEmitter::Fail(llvm::Value* status) {
  ReleaseBorrowedHostFrame();
  AbortConstructions();
  if (kind_ == NativeEntryKind::Leaf) {
    // 内部叶入口的失败结果复用 ABI 暂存槽保存源位置；此处禁止分配和进入运行时。
    StoreMemory(builder_.getInt64(TracePosition()), Output(), MemoryRegion::Values);
    Finish(status);
    return;
  }
  if (leaf_entry_ || deferred_roots_) {
    if (!leaf_entry_) {
      SetPosition();
    }
    const auto& live = roots_.stores.at(instruction_index_);
    for (std::size_t index = 0; index < live.size(); ++index) {
      StoreRootValue(live.at(index), RootAddress(index));
    }
    auto* roots = live.empty() ? llvm::ConstantPointerNull::get(builder_.getPtrTy()) : root_slots_;
    if (leaf_entry_) {
      Finish(EmitLeafFailure({
          .status = status,
          .position = builder_.getInt64(TracePosition()),
          .roots = roots,
          .count = live.size(),
      }));
    } else {
      auto failure = module_.getOrInsertFunction(
          runtime::kNativeUnrootedFailureSymbol, builder_.getInt8Ty(), builder_.getPtrTy(),
          builder_.getInt8Ty(), builder_.getPtrTy(), builder_.getInt64Ty());
      Finish(builder_.CreateCall(
          failure, {body_->getArg(0), status, roots, builder_.getInt64(live.size())}));
    }
    return;
  }
  SetPosition();
  std::optional<std::uint32_t> handler;
  if (!source_.instructions.empty()) {
    // 失败封套可以分配；条件式运行时调用的成功分支可能尚未登记当前活动值。
    StoreRoots(instruction_index_);
    handler = source_.instructions.at(instruction_index_).handler;
  }
  auto failure = module_.getOrInsertFunction(runtime::kNativeFailureSymbol, builder_.getInt8Ty(),
                                             builder_.getPtrTy(), builder_.getInt8Ty());
  auto* propagated = builder_.CreateCall(failure, {body_->getArg(0), status});
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
        StoreMemory(result, Output(), MemoryRegion::Values);
      }
      Finish(Status(builder_, ExecutionStatus::Returned));
      break;
    }
    default:
      return EmitReference(instruction);
  }
  return {};
}

const NativeFrameLayout& FunctionEmitter::PlanFrame() {
  if (frame_plan_) {
    return *frame_plan_;
  }
  NativeFrameRequirements requirements{.recovery = HasRecovery()};
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    const auto* member = reflection_members_.empty() ? nullptr : reflection_members_[index];
    if (IsLeafCall(CallTarget(index), index) ||
        (member != nullptr && member->kind == reflection::MemberKind::Method &&
         generated_[member->target].leaf != nullptr)) {
      requirements.leaf_calls = true;
      break;
    }
  }
  frame_plan_ = PlanNativeFrame(module_, escape_, roots_, temporary_count_, requirements, kind_,
                                PlanAuxiliaryStorage());
  return *frame_plan_;
}

template <typename Address>
void FunctionEmitter::BindFrameSlots(const Address& address) {
  arguments_ = address(temporary_field_, "", std::nullopt);
  result_ = address(temporary_field_ + 1, "", std::nullopt);
  if (auxiliary_field_) {
    auxiliary_storage_ = address(*auxiliary_field_, "", std::nullopt);
  }
  if (leaf_result_field_) {
    leaf_result_ = address(*leaf_result_field_, "leaf.result", std::nullopt);
  }
  if (roots_.debug_values) {
    debug_values_ =
        address(frame_type_->getNumElements() - 1, "", alignof(runtime::NativeDebugValue));
  }
  if (HasRecovery()) {
    checkpoint_ = address(temporary_field_ + 2, "", alignof(runtime::NativeCheckpoint));
    checkpoint_values_ = address(temporary_field_ + 3, "", std::nullopt);
    recovery_storage_ = address(temporary_field_ + 4, "", std::nullopt);
    if (!escape_.checkpoints.empty()) {
      virtual_references_ =
          address(temporary_field_ + kVirtualReferenceFieldIndex, "", std::nullopt);
      virtual_state_ = address(temporary_field_ + kVirtualStateFieldIndex, "",
                               alignof(runtime::NativeVirtualState));
    }
  }
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
  const auto& frame = PlanFrame();
  frame_type_ = frame.type;
  frame_locals_ = frame.local_fields;
  temporary_field_ = frame.temporary_field;
  leaf_result_field_ = frame.leaf_result_field;
  auxiliary_field_ = frame.auxiliary_field;
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
    // 纯计算结果独占 alloca；不与传入运行时的 ABI 暂存槽共享地址，以便提升为 SSA。
    BindFrameSlots([&](unsigned field, const char* name, std::optional<std::size_t> alignment) {
      auto* slot = allocation.CreateAlloca(frame_type_->getElementType(field), nullptr, name);
      if (alignment) {
        slot->setAlignment(llvm::Align(*alignment));
      }
      return slot;
    });
  }
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
  BindFrameSlots([&](unsigned field, const char*, std::optional<std::size_t>) {
    return builder_.CreateStructGEP(frame_type_, frame_storage_, field);
  });
}

std::expected<void, std::string> FunctionEmitter::EmitInstructionBlock(std::size_t index) {
  const auto& instruction = source_.instructions.at(index);
  instruction_index_ = index;
  builder_.SetInsertPoint(blocks_.at(index));
  const auto safepoint = IsBoundaryPollRequired(index);
  boundary_ready_ = nullptr;
  if (safepoint && kind_ != NativeEntryKind::Leaf && kind_ != NativeEntryKind::BoundedReflection) {
    boundary_ready_ = Poll();
  }
  if (runtime_calls_[index] != 0) {
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
  if (!builder_.GetInsertBlock()->hasTerminator()) {
    ClearRoots(index);
    builder_.CreateBr(blocks_.at(index + 1));
  }
  return {};
}

void FunctionEmitter::BindArguments() {
  if (kind_ == NativeEntryKind::Initial || kind_ == NativeEntryKind::BoundedReflection ||
      kind_ == NativeEntryKind::Borrowed || kind_ == NativeEntryKind::Leaf) {
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
  if (IsClosedLinkedProgram(program_.native_mode, placement_) && roots_.bounded_polls &&
      ir::CanAnalyzeSynchronousFunction(source_)) {
    body_->addFnAttr(kNativeArrayReductionCandidateAttribute);
  }
  auto* entry = llvm::BasicBlock::Create(context, "entry", body_);
  builder_.SetInsertPoint(entry);
  if (kind_ == NativeEntryKind::Resume) {
    EmitExecutionBoundary(builder_);
  }
  if (program_.native_mode == NativeProgramMode::Reloadable) {
    image_ = LoadProgramImage(builder_, body_->getArg(0));
  }
  GuardResume();
  CacheReflectionMembers();
  PrepareFrame();
  leaf_entry_ = CanUseLeafEntry();
  deferred_roots_ = CanDeferRoots();
  EmitFrameEntry();
  BindFrameStorage();
  InitializeCheckpoint();
  CacheImports();
  BorrowHostExecution();
  CacheConstants();
  CacheNativeStringViews();
  CacheMapLookups();
  CacheRuntimeCalls();
  CacheFixedArrayStorage();
  CacheGlobals();
  CacheFields();
  BindArguments();
  if (kind_ == NativeEntryKind::Leaf && generated_[function_index_].budgeted_recursion) {
    auto* budget = body_->getArg(LeafEntryLayout(source_.parameter_count).budget);
    auto* exhausted = llvm::BasicBlock::Create(context, "recursion.exhausted", body_);
    auto* ready = llvm::BasicBlock::Create(context, "recursion.ready", body_);
    builder_.CreateCondBr(builder_.CreateICmpEQ(budget, builder_.getInt32(0)), exhausted, ready);
    builder_.SetInsertPoint(exhausted);
    Finish(builder_.getInt8(kNativeLeafRetry));
    builder_.SetInsertPoint(ready);
  }
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
                           NativeDebugging debugging,
                           std::span<const std::uint8_t> readonly_globals) {
  const auto& function = escape.function.get();
  std::vector<std::vector<ir::RegisterId>> required;
  std::vector<std::uint8_t> safepoints;
  std::vector<ir::RegisterId> materialized;
  NativeRecoveryRequirements requirements;
  if (!escape.checkpoints.empty()) {
    for (const auto& allocation : escape.allocations) {
      if (allocation.can_scalar_replace) {
        materialized.push_back(allocation.reference);
      }
    }
    const auto& canonical = program.functions.at(function_index);
    const auto canonical_roots = PlanNativeRoots(canonical, program.functions, program.native_mode,
                                                 debugging, {}, readonly_globals);
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
        .materialized = materialized,
        .minimum_root_count = canonical_roots.count,
        .minimum_temporary_count = temporary,
        .uniform_values = {},
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
  return PlanNativeRoots(function, program.functions, program.native_mode, debugging, requirements,
                         readonly_globals);
}

std::vector<std::optional<reflection::Operation>> PlanReflectionOperations(
    const ir::Function& function,
    std::span<const ir::Function> functions);

FunctionEmissionFacts PlanEmissionFacts(const ir::Program& program,
                                        const NativeCallPlan& calls,
                                        const FieldTargets& field_targets,
                                        ArrayEmissionFacts array_access,
                                        const GlobalAccessFacts& global_access,
                                        std::size_t function_index,
                                        const EscapePlan& escape,
                                        const NativeRootPlan& roots,
                                        NativeCodeTier tier,
                                        NativeCodePlacement placement) {
  const auto& function = escape.function.get();
  FunctionEmissionFacts facts{
      .placement = placement,
      .field_targets = &field_targets,
      .temporary_count = PlanTemporaryCount(function, program, roots),
      .suspension_capacity = 0,
      .field_accesses = {},
      .field_kind_writes = FindFieldKindWrites(function, program, roots.safepoints),
      .direct_calls = {},
      .reflection_operations = PlanReflectionOperations(function, program.functions),
      .arrays = {},
      .global_access = &global_access,
  };
  for (const auto& suspended : roots.suspended) {
    facts.suspension_capacity = std::max(facts.suspension_capacity, suspended.size());
  }
  const auto& canonical = program.functions.at(function_index);
  if (roots.bounded_polls) {
    if (IsClosedLinkedProgram(program.native_mode, placement) && !canonical.is_async) {
      // 同步帧内的存储描述随引用一起传播；挂起帧仍只保存语言层寄存器。
      facts.arrays = array_access;
    } else {
      facts.arrays.element_bits = array_access.element_bits;
    }
  }
  if (tier == NativeCodeTier::Optimized) {
    // 规范 IR 复用调用计划；逃逸改写后的寄存器与指令位置必须重新分析。
    facts.direct_calls = &function == &canonical ? calls.direct_calls[function_index]
                                                 : FindDirectCalls(function, program);
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

std::vector<std::optional<reflection::Operation>> PlanReflectionOperations(
    const ir::Function& function,
    std::span<const ir::Function> functions) {
  if (std::ranges::none_of(function.instructions, [](const auto& instruction) {
        return ir::IsBuiltinCall(instruction, Builtin::Reflection) ||
               ir::IsBuiltinCall(instruction, Builtin::ReflectionInvoke);
      })) {
    return {};
  }
  const auto accesses = ir::AnalyzeRegisterAccesses(function, functions);
  const auto flow = ir::AnalyzeRegisterFlow(function, accesses);
  std::vector<std::optional<reflection::Operation>> operations(function.instructions.size());
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions[index];
    if (!ir::IsBuiltinCall(instruction, Builtin::Reflection) &&
        !ir::IsBuiltinCall(instruction, Builtin::ReflectionInvoke)) {
      continue;
    }
    const auto definition = flow.OriginDefinition(instruction.lhs);
    if (definition && function.instructions[*definition].opcode == Opcode::LoadInt) {
      operations[index] =
          static_cast<reflection::Operation>(function.instructions[*definition].immediate);
    }
  }
  return operations;
}

}  // namespace

NativeFunctionAnalysis PlanFunctionAnalysis(
    const ir::Program& program,
    std::size_t function_index,
    NativeDebugging debugging,
    NativeCodeTier tier,
    EscapeOptimization optimization,
    std::span<const std::uint8_t> readonly_globals,
    std::span<const std::optional<ir::ClassId>> borrowable) {
  auto escape = PlanEscapes(program, function_index, optimization, tier, borrowable);
  auto roots = EscapeRoots(program, function_index, escape, debugging, readonly_globals);
  return {.escape = std::move(escape), .roots = std::move(roots)};
}

std::expected<NativeFunctionMetrics, std::string> EmitFunction(
    llvm::Module& module,
    const ir::Program& program,
    const NativeCallPlan& calls,
    const FieldTargets& field_targets,
    ArrayEmissionFacts array_access,
    const GlobalAccessFacts& global_access,
    llvm::Constant* image,
    std::size_t function_index,
    const NativeFunctionAnalysis& analysis,
    std::span<const GeneratedFunction> generated,
    std::vector<std::size_t>& body_dependencies,
    NativeDebugging debugging,
    NativeCodePlacement placement,
    NativeCodeTier tier) {
  const auto& escape = analysis.escape;
  const auto& roots = analysis.roots;
  const auto& function = generated[function_index];
  const auto facts = PlanEmissionFacts(program, calls, field_targets, array_access, global_access,
                                       function_index, escape, roots, tier, placement);
  FunctionEmitter initial(module, program, calls, image, function_index, escape, generated, roots,
                          facts, NativeEntryKind::Initial, tier);
  if (std::ranges::any_of(escape.allocations, &AllocationSite::stack_record)) {
    const auto& frame = initial.PlanFrame();
    if (frame.bytes > kMaximumNativeStackBytes) {
      // 在 LLVM body 发射前完成表示选择；超预算记录使用普通调用所需的堆物化。
      const auto scalar = PlanFunctionAnalysis(program, function_index, debugging, tier,
                                               EscapeOptimization::ScalarReplacement,
                                               global_access.readonly_slots, {});
      return EmitFunction(module, program, calls, field_targets, array_access, global_access, image,
                          function_index, scalar, generated, body_dependencies, debugging,
                          placement, tier);
    }
  }
  body_dependencies.insert(body_dependencies.end(), escape.body_dependencies.begin(),
                           escape.body_dependencies.end());
  if (const auto emitted = initial.Emit(); !emitted) {
    return std::unexpected(emitted.error());
  }
  NativeFunctionMetrics metrics{.shadow_slots = roots.count, .frame_bytes = initial.frame_bytes()};
  AccumulateMetrics(metrics, escape, roots);
  if (tier == NativeCodeTier::Optimized && function.bounded_reflection_body != nullptr) {
    FunctionEmitter reflected(module, program, calls, image, function_index, escape, generated,
                              roots, facts, NativeEntryKind::BoundedReflection, tier);
    if (const auto emitted = reflected.Emit(); !emitted) {
      return std::unexpected(emitted.error());
    }
    metrics.frame_bytes = std::max(metrics.frame_bytes, reflected.frame_bytes());
  }
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
    ArrayEmissionFacts array_access,
    const GlobalAccessFacts& global_access,
    llvm::Constant* image,
    std::size_t function_index,
    const EscapePlan& plan,
    std::span<const GeneratedFunction> generated,
    NativeDebugging debugging,
    NativeCodePlacement placement) {
  const auto& function = plan.function.get();
  const auto roots = PlanNativeRoots(
      function, program.functions, program.native_mode, debugging,
      {.values = {}, .safepoints = {}, .materialized = {}, .uniform_values = plan.borrowed_fields},
      global_access.readonly_slots);
  const auto facts =
      PlanEmissionFacts(program, calls, field_targets, array_access, global_access, function_index,
                        plan, roots, NativeCodeTier::Baseline, placement);
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
