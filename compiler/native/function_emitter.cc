#include "compiler/native/function_emitter.h"
#include "base/builtin.h"
#include "base/native_program_mode.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/native_frame_plan.h"
#include "compiler/native/native_roots.h"
#include "compiler/native/native_values.h"

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

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/AtomicOrdering.h>

#include "base/execution_status.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_deoptimization.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {
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
  return HasValidity(source_.value_types.at(reg)) ? builder_.CreateExtractValue(value, 1) : value;
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
  const auto position = origins_.empty() ? 0 : origins_.subspan(instruction_index_).front();
  builder_.CreateStore(builder_.getInt32(static_cast<std::uint32_t>(position)), instruction_slot_);
}

void FunctionEmitter::PrepareRuntimeCall() {
  if (leaf_entry_) {
    return;
  }
  SetPosition();
  StoreRoots(instruction_index_);
  StoreCheckpoint();
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
      .instruction = static_cast<std::uint32_t>(origins_.subspan(instruction_index_).front()),
  });
}

void FunctionEmitter::Poll() {
  auto& context = module_.getContext();
  auto* requests_address = builder_.CreateLoad(
      builder_.getPtrTy(),
      ByteAddress(body_->getArg(0), runtime::NativeMemoryLayout::kExecutionSafepointRequests));
  // 请求字属于 Isolate，地址在整个执行期间固定；请求值仍必须在每个安全点原子读取。
  requests_address->setMetadata(llvm::LLVMContext::MD_invariant_load,
                                llvm::MDNode::get(context, {}));
  auto* requests = builder_.CreateLoad(builder_.getInt8Ty(), requests_address);
  requests->setAtomic(llvm::AtomicOrdering::Acquire);
  llvm::Value* required = builder_.CreateICmpNE(requests, builder_.getInt8(0));
  if (tier_ == NativeCodeTier::Optimized) {
    // 被调用方可能已消耗发布请求并使当前帧失效；返回后仍须在下一安全点恢复。
    auto* tier = builder_.CreateLoad(
        builder_.getInt8Ty(), ByteAddress(checkpoint_, offsetof(runtime::NativeCheckpoint, tier)));
    required = builder_.CreateOr(
        required, builder_.CreateICmpEQ(
                      tier, builder_.getInt8(static_cast<std::uint8_t>(NativeCodeTier::Baseline))));
  }
  auto* process = llvm::BasicBlock::Create(context, "safepoint.process", body_);
  auto* ready = llvm::BasicBlock::Create(context, "safepoint.ready", body_);
  builder_.CreateCondBr(required, process, ready,
                        llvm::MDBuilder(context).createUnlikelyBranchWeights());
  builder_.SetInsertPoint(process);
  SetPosition();
  if (source_.native_symbol.empty()) {
    StoreRoots(instruction_index_);
    StoreCheckpoint();
  }
  StoreDebugValues();
  auto poll = module_.getOrInsertFunction("vela_native_v3_safepoint", builder_.getInt8Ty(),
                                          builder_.getPtrTy());
  CheckStatus(builder_.CreateCall(poll, {body_->getArg(0)}));
  if (tier_ == NativeCodeTier::Optimized) {
    RecoverIfInvalidated();
  }
  builder_.CreateBr(ready);
  builder_.SetInsertPoint(ready);
}

void FunctionEmitter::Fail(llvm::Value* status) {
  AbortConstructions();
  if (leaf_entry_) {
    const auto& live = roots_.stores.at(instruction_index_);
    for (std::size_t index = 0; index < live.size(); ++index) {
      WriteHeapValue(RootAddress(index), live.at(index));
    }
    auto failure = module_.getOrInsertFunction(
        "vela_native_v2_leaf_failure", builder_.getInt8Ty(), builder_.getPtrTy(),
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
  auto failure = module_.getOrInsertFunction("vela_native_v1_failure", builder_.getInt8Ty(),
                                             builder_.getPtrTy(), builder_.getInt8Ty());
  auto* propagated = builder_.CreateCall(failure, {body_->getArg(0), status});
  const auto handler = source_.instructions.empty()
                           ? std::optional<std::uint32_t>()
                           : source_.instructions.at(instruction_index_).handler;
  if (handler) {
    auto pending = module_.getOrInsertFunction("vela_native_v1_has_exception", builder_.getInt8Ty(),
                                               builder_.getPtrTy());
    auto* has_exception = builder_.CreateCall(pending, {body_->getArg(0)});
    auto* unhandled = llvm::BasicBlock::Create(module_.getContext(), "unhandled", body_);
    // handler 的下一安全点会登记其活动值；此处到 handler 之间不再分配。
    // 批量清空根区，避免每个失败点展开整个函数的根槽，生成平方规模的 LLVM IR。
    if (roots_.count != 0) {
      static_assert(static_cast<std::uint8_t>(runtime::ValueKind::Null) == 0);
      builder_.CreateMemSet(root_slots_, builder_.getInt8(0),
                            roots_.count * sizeof(runtime::Value),
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
    case Opcode::AndBool:
    case Opcode::OrBool: {
      auto* left = Payload(instruction.lhs);
      auto* right = Payload(instruction.rhs);
      auto* result = instruction.opcode == Opcode::AndBool ? builder_.CreateAnd(left, right)
                                                           : builder_.CreateOr(left, right);
      Store(instruction.dst, result, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
      break;
    }
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
      EmitCall(instruction);
      break;
    case Opcode::Await:
      EmitAwait(instruction);
      break;
    case Opcode::NewFuture:
    case Opcode::SleepFuture:
    case Opcode::AsyncCall:
      return EmitAsyncInstruction(instruction);
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

std::expected<void, std::string> FunctionEmitter::PrepareFrame() {
  if (source_.value_types.size() != source_.register_count) {
    return std::unexpected("native code requires upstream virtual value types");
  }
  slots_.resize(source_.register_count);
  if (std::ranges::any_of(source_.instructions, [](const auto& instruction) {
        return instruction.construction.has_value();
      })) {
    construction_depth_ = builder_.CreateAlloca(builder_.getInt32Ty());
    builder_.CreateStore(builder_.getInt32(0), construction_depth_);
  }
  const auto frame = PlanNativeFrame(module_, program_, escape_, roots_, tier_, kind_);
  if (!frame) {
    return std::unexpected(frame.error());
  }
  frame_type_ = frame->type;
  const std::vector<llvm::Type*> fields(frame_type_->element_begin(), frame_type_->element_end());
  storage_bytes_ = module_.getDataLayout().getTypeAllocSize(frame_type_);
  frame_bytes_ = frame->bytes;
  if (!IsRecovering() && frame_bytes_ <= kMaximumNativeStackBytes) {
    // 局部值的地址不传给 helper；独立 alloca 可提升为 SSA。将它们与 ABI 暂存区
    // 放在同一结构中会让暂存区的地址逃逸阻止提升，导致纯算术循环持续读写栈。
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      if (source_.value_types.at(index).kind != Kind::Invalid) {
        slots_.at(index) = builder_.CreateAlloca(fields.at(index));
      }
    }
    arguments_ = builder_.CreateAlloca(fields.at(slots_.size()));
    result_ = builder_.CreateAlloca(fields.at(slots_.size() + 1));
    if (roots_.debug_values) {
      auto* values = builder_.CreateAlloca(fields.back());
      values->setAlignment(llvm::Align(alignof(runtime::NativeDebugValue)));
      debug_values_ = values;
    }
    if (tier_ == NativeCodeTier::Optimized) {
      auto* checkpoint = builder_.CreateAlloca(fields.at(slots_.size() + 2));
      checkpoint->setAlignment(llvm::Align(alignof(runtime::NativeCheckpoint)));
      checkpoint_ = checkpoint;
      checkpoint_values_ = builder_.CreateAlloca(fields.at(slots_.size() + 3));
      recovery_storage_ = builder_.CreateAlloca(fields.at(slots_.size() + 4));
      if (!escape_.checkpoints.empty()) {
        virtual_references_ =
            builder_.CreateAlloca(fields.at(slots_.size() + kVirtualReferenceFieldIndex));
      }
    }
  }
  return {};
}

bool FunctionEmitter::CanUseLeafEntry() const {
  if (roots_.debug_values || program_.native_mode == NativeProgramMode::Reloadable || source_.is_async ||
      !source_.native_symbol.empty() || !source_.handlers.empty() ||
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
    if (source_.value_types.at(index).kind != Kind::Invalid) {
      slots_.at(index) =
          builder_.CreateStructGEP(frame_type_, frame_storage_, static_cast<unsigned>(index));
    }
  }
  const auto scratch = static_cast<unsigned>(slots_.size());
  arguments_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch);
  result_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + 1);
  if (roots_.debug_values) {
    debug_values_ = builder_.CreateStructGEP(frame_type_, frame_storage_, frame_type_->getNumElements() - 1);
  }
  if (tier_ == NativeCodeTier::Optimized) {
    checkpoint_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + 2);
    checkpoint_values_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + 3);
    recovery_storage_ = builder_.CreateStructGEP(frame_type_, frame_storage_, scratch + 4);
    if (!escape_.checkpoints.empty()) {
      virtual_references_ = builder_.CreateStructGEP(frame_type_, frame_storage_,
                                                     scratch + kVirtualReferenceFieldIndex);
    }
  }
}

void FunctionEmitter::EmitImport() {
  using AotCallKind = runtime::NativeFunction::AotCallKind;
  using runtime::NativeMemoryLayout;
  auto* context = body_->getArg(0);
  auto* function = builder_.getInt32(static_cast<std::uint32_t>(function_index_));
  std::vector<llvm::Value*> arguments{context, function};
  std::vector<llvm::Type*> types{builder_.getPtrTy(), builder_.getInt32Ty()};
  for (std::size_t index = 0; index < source_.parameter_count; ++index) {
    const auto reg = static_cast<ir::RegisterId>(index);
    if (const auto slot = roots_.slots.at(reg)) {
      WriteHeapValue(RootAddress(*slot), reg);
    }
    auto* value = Load(reg);
    if (source_.value_types.at(reg).kind == Kind::Bool) {
      value = builder_.CreateZExt(value, builder_.getInt8Ty());
    }
    arguments.push_back(value);
    types.push_back(value->getType());
  }
  const auto host_boundary = [&](const char* symbol) {
    if (program_.native_mode == NativeProgramMode::Reloadable) {
      auto helper = module_.getOrInsertFunction(symbol, builder_.getVoidTy(), builder_.getPtrTy());
      builder_.CreateCall(helper, {context});
    }
  };
  if (program_.native_mode == NativeProgramMode::Reloadable || roots_.debug_values) {
    Poll();
  }
  EmitIntrinsicImport();
  const auto& return_type = source_.return_type;
  auto* result_type = return_type.kind == Kind::Bool || return_type.kind == Kind::Void
                          ? builder_.getInt8Ty()
                          : ValueType(module_.getContext(), return_type);
  auto* entries = body_->getArg(static_cast<unsigned>(source_.parameter_count + 1));
  auto* target = builder_.CreateExtractValue(entries, 0);
  auto* call_kind = builder_.CreateExtractValue(entries, 1);
  const auto emit_direct = [&](AotCallKind kind) {
    std::vector<llvm::Value*> direct_arguments(arguments.begin() + 2, arguments.end());
    std::vector<llvm::Type*> direct_types(types.begin() + 2, types.end());
    if (kind == AotCallKind::DirectWithContext) {
      direct_arguments.insert(direct_arguments.begin(),
                              ByteAddress(context, NativeMemoryLayout::kExecutionContext));
      direct_types.insert(direct_types.begin(), builder_.getPtrTy());
    }
    auto* direct_result = return_type.kind == Kind::Void ? builder_.getVoidTy() : result_type;
    auto* signature = llvm::FunctionType::get(direct_result, direct_types, false);
    host_boundary("vela_native_v3_host_enter");
    auto* value = builder_.CreateCall(signature, target, direct_arguments);
    host_boundary("vela_native_v3_host_exit");
    if (return_type.kind != Kind::Void) {
      llvm::Value* result = value;
      if (return_type.kind == Kind::Bool) {
        result = builder_.CreateICmpNE(result, builder_.getInt8(0));
      }
      builder_.CreateStore(result, Output());
    }
    Finish(Status(builder_, ExecutionStatus::Returned));
  };
  if (return_type.kind == Kind::Void || return_type.kind == Kind::Bool ||
      return_type.kind == Kind::Int || return_type.kind == Kind::Double) {
    auto* direct = llvm::BasicBlock::Create(module_.getContext(), "import.direct", body_);
    auto* checked = llvm::BasicBlock::Create(module_.getContext(), "import.checked", body_);
    builder_.CreateCondBr(
        builder_.CreateICmpNE(call_kind,
                              builder_.getInt8(static_cast<std::uint8_t>(AotCallKind::Checked))),
        direct, checked);
    builder_.SetInsertPoint(direct);
    auto* with_context = llvm::BasicBlock::Create(module_.getContext(), "import.context", body_);
    auto* plain = llvm::BasicBlock::Create(module_.getContext(), "import.plain", body_);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(
            call_kind, builder_.getInt8(static_cast<std::uint8_t>(AotCallKind::DirectWithContext))),
        with_context, plain);
    builder_.SetInsertPoint(with_context);
    emit_direct(AotCallKind::DirectWithContext);
    builder_.SetInsertPoint(plain);
    emit_direct(AotCallKind::Direct);
    builder_.SetInsertPoint(checked);
  }
  arguments.push_back(Output());
  types.push_back(builder_.getPtrTy());
  auto* signature = llvm::FunctionType::get(builder_.getInt8Ty(), types, false);
  host_boundary("vela_native_v3_host_enter");
  auto* called = builder_.CreateCall(signature, target, arguments);
  host_boundary("vela_native_v3_host_exit");
  CheckStatus(called);
  if (return_type.kind != Kind::Void) {
    llvm::Value* value = builder_.CreateLoad(result_type, Output());
    if (return_type.kind == Kind::Bool) {
      value = builder_.CreateICmpNE(value, builder_.getInt8(0));
    }
    builder_.CreateStore(value, Output());
  }
  Finish(Status(builder_, ExecutionStatus::Returned));
}

std::expected<void, std::string> FunctionEmitter::EmitInstructionBlock(std::size_t index) {
  const auto& instruction = source_.instructions.at(index);
  instruction_index_ = index;
  builder_.SetInsertPoint(blocks_.at(index));
  const auto effects = ir::AnalyzeEffects(instruction);
  const auto safepoint = roots_.safepoints.at(index) != 0;
  if (safepoint) {
    Poll();
  }
  const auto conditional_call =
      (instruction.opcode == Opcode::BuiltinCall &&
       instruction.index == static_cast<std::uint32_t>(Builtin::ArrayPush)) ||
      (instruction.opcode == Opcode::GetField && instruction.field_declaration.has_value()) ||
      (instruction.opcode == Opcode::GetGlobal &&
       program_.native_mode == NativeProgramMode::Closed);
  if (effects.MayCollectOnSuccess() && !conditional_call) {
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
  if (kind_ == NativeEntryKind::Initial || kind_ == NativeEntryKind::Borrowed) {
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
  const auto& canonical = program_.functions.at(function_index_).instructions;
  field_accesses_.resize(canonical.size());
  std::uint32_t access = 0;
  for (std::size_t index = 0; index < canonical.size(); ++index) {
    if (canonical.at(index).field_declaration) {
      field_accesses_.at(index) = access++;
    }
  }
  auto* entry = llvm::BasicBlock::Create(context, "entry", body_);
  builder_.SetInsertPoint(entry);
  GuardResume();
  const auto prepared = PrepareFrame();
  if (!prepared) {
    return prepared;
  }
  leaf_entry_ = CanUseLeafEntry();
  EmitFrameEntry();
  BindFrameStorage();
  InitializeCheckpoint();
  InitializeDebugValues();
  CacheImports();
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

std::expected<NativeRootPlan, std::string> EscapeRoots(const ir::Program& program,
                                                       std::size_t function_index,
                                                       const EscapePlan& escape,
                                                       NativeDebugging debugging) {
  std::vector<std::vector<ir::RegisterId>> required;
  std::vector<std::uint8_t> safepoints;
  NativeRecoveryRequirements requirements;
  if (!escape.checkpoints.empty()) {
    const auto& canonical = program.functions.at(function_index);
    const auto canonical_roots =
        PlanNativeRoots(canonical, program.functions, program.native_mode, debugging);
    const auto temporary = PlanTemporaryCount(canonical, program, canonical_roots);
    if (!temporary) {
      return std::unexpected(temporary.error());
    }
    required.resize(escape.function.instructions.size());
    safepoints.resize(escape.function.instructions.size());
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
        .minimum_temporary_count = *temporary,
    };
  }
  if (std::ranges::any_of(escape.borrowed_calls,
                          [](const auto& call) { return call.has_value(); })) {
    required.resize(escape.function.instructions.size());
    for (std::size_t index = 0; index < escape.borrowed_calls.size(); ++index) {
      if (const auto object = escape.borrowed_calls.at(index)) {
        required.at(index) = escape.allocations.at(*object).fields;
      }
    }
    requirements.values = required;
  }
  return PlanNativeRoots(escape.function, program.functions, program.native_mode, debugging,
                         requirements);
}

void AccumulateMetrics(NativeFunctionMetrics& metrics,
                       const EscapePlan& escape,
                       const NativeRootPlan& roots) {
  for (const auto& allocation : escape.allocations) {
    metrics.stack_records += allocation.stack_record ? 1 : 0;
    if (allocation.decision == AllocationDecision::ScalarReplacement) {
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
    llvm::Constant* image,
    std::size_t function_index,
    std::span<const GeneratedFunction> generated,
    NativeDebugging debugging,
    NativeCodeTier tier,
    EscapeOptimization optimization,
    std::span<const std::optional<ir::ClassId>> borrowable) {
  const auto escape = PlanEscapes(program, function_index, optimization, tier, borrowable);
  const auto planned = EscapeRoots(program, function_index, escape, debugging);
  if (!planned) {
    return std::unexpected(planned.error());
  }
  const auto& roots = *planned;
  if (std::ranges::any_of(escape.allocations, &AllocationSite::stack_record)) {
    const auto frame =
        PlanNativeFrame(module, program, escape, roots, tier, NativeEntryKind::Initial);
    if (!frame) {
      return std::unexpected(frame.error());
    }
    if (frame->bytes > kMaximumNativeStackBytes) {
      // 在 LLVM body 发射前完成表示选择；超预算记录使用普通调用所需的堆物化。
      return EmitFunction(module, program, image, function_index, generated, debugging, tier,
                          EscapeOptimization::ScalarReplacement);
    }
  }
  FunctionEmitter initial(module, program, image, function_index, escape, generated, roots,
                          NativeEntryKind::Initial, tier);
  if (const auto emitted = initial.Emit(); !emitted) {
    return std::unexpected(emitted.error());
  }
  NativeFunctionMetrics metrics{.shadow_slots = roots.count, .frame_bytes = initial.frame_bytes()};
  AccumulateMetrics(metrics, escape, roots);
  const auto& function = generated.subspan(function_index).front();
  if (function.resume != nullptr) {
    FunctionEmitter resume(module, program, image, function_index, escape, generated, roots,
                           NativeEntryKind::Resume, tier);
    if (const auto emitted = resume.Emit(); !emitted) {
      return std::unexpected(emitted.error());
    }
    metrics.frame_bytes = std::max(metrics.frame_bytes, resume.frame_bytes());
  }
  if (tier == NativeCodeTier::Baseline && function.recovery != nullptr) {
    FunctionEmitter recovery(module, program, image, function_index, escape, generated, roots,
                             NativeEntryKind::RecoverInitial, tier);
    if (const auto emitted = recovery.Emit(); !emitted) {
      return std::unexpected(emitted.error());
    }
    if (function.resume_recovery != nullptr) {
      FunctionEmitter resume(module, program, image, function_index, escape, generated, roots,
                             NativeEntryKind::RecoverResume, tier);
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
    llvm::Constant* image,
    std::size_t function_index,
    const EscapePlan& plan,
    std::span<const GeneratedFunction> generated,
    NativeDebugging debugging) {
  const std::vector<std::vector<ir::RegisterId>> required(plan.function.instructions.size(),
                                                          plan.borrowed_fields);
  const auto roots = PlanNativeRoots(plan.function, program.functions, program.native_mode,
                                     debugging, {.values = required, .safepoints = {}});
  FunctionEmitter borrowed(module, program, image, function_index, plan, generated, roots,
                           NativeEntryKind::Borrowed, NativeCodeTier::Baseline);
  if (const auto emitted = borrowed.Emit(); !emitted) {
    return std::unexpected(emitted.error());
  }
  NativeFunctionMetrics metrics{.shadow_slots = roots.count, .frame_bytes = borrowed.frame_bytes()};
  AccumulateMetrics(metrics, plan, roots);
  return metrics;
}

}  // namespace vela::compiler::native
