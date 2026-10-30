#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/native/function_emitter.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>

#include "compiler/native/native_values.h"
#include "runtime/native_abi.h"
#include "runtime/native_deoptimization.h"

namespace vela::compiler::native {

llvm::Function* FunctionEmitter::SelectBody(const GeneratedFunction& function,
                                            NativeEntryKind kind) {
  switch (kind) {
    case NativeEntryKind::Initial:
      return function.body;
    case NativeEntryKind::Resume:
      return function.resume;
    case NativeEntryKind::RecoverInitial:
      return function.recovery;
    case NativeEntryKind::RecoverResume:
      return function.resume_recovery;
  }
  std::unreachable();
}

bool FunctionEmitter::IsRecovering() const noexcept {
  return kind_ == NativeEntryKind::RecoverInitial || kind_ == NativeEntryKind::RecoverResume;
}

bool FunctionEmitter::IsResuming() const noexcept {
  return kind_ == NativeEntryKind::Resume || kind_ == NativeEntryKind::RecoverResume;
}

llvm::Value* FunctionEmitter::ResumeState() {
  if (kind_ == NativeEntryKind::Resume) {
    return body_->getArg(1);
  }
  return builder_.CreateLoad(
      builder_.getPtrTy(),
      ByteAddress(body_->getArg(1), offsetof(runtime::NativeCheckpoint, resume)));
}

void FunctionEmitter::GuardResume() {
  if (tier_ != NativeCodeTier::Optimized || kind_ != NativeEntryKind::Resume) {
    return;
  }
  auto helper = module_.getOrInsertFunction("vela_native_v4_can_optimize", builder_.getInt8Ty(),
                                            builder_.getPtrTy(), builder_.getPtrTy());
  auto* current = builder_.CreateCall(helper, {body_->getArg(0), image_});
  auto* optimized = llvm::BasicBlock::Create(module_.getContext(), "optimized", body_);
  auto* baseline = llvm::BasicBlock::Create(module_.getContext(), "baseline", body_);
  builder_.CreateCondBr(builder_.CreateICmpNE(current, builder_.getInt8(0)), optimized, baseline);
  builder_.SetInsertPoint(baseline);
  auto* resume = generated_.subspan(function_index_).front().baseline_resume;
  builder_.CreateRet(builder_.CreateCall(resume, {body_->getArg(0), body_->getArg(1)}));
  builder_.SetInsertPoint(optimized);
}

void FunctionEmitter::InitializeCheckpoint() {
  if (checkpoint_ == nullptr) {
    return;
  }
  using runtime::NativeCheckpoint;
  const auto& function = generated_.subspan(function_index_).front();
  builder_.CreateStore(checkpoint_values_,
                       ByteAddress(checkpoint_, offsetof(NativeCheckpoint, values)));
  auto* roots =
      roots_.count == 0 ? llvm::ConstantPointerNull::get(builder_.getPtrTy()) : root_slots_;
  builder_.CreateStore(roots, ByteAddress(checkpoint_, offsetof(NativeCheckpoint, roots)));
  builder_.CreateStore(recovery_storage_,
                       ByteAddress(checkpoint_, offsetof(NativeCheckpoint, storage)));
  llvm::Value* resume =
      IsResuming() ? ResumeState() : llvm::ConstantPointerNull::get(builder_.getPtrTy());
  builder_.CreateStore(resume, ByteAddress(checkpoint_, offsetof(NativeCheckpoint, resume)));
  builder_.CreateStore(IsResuming() ? function.resume_recovery : function.recovery,
                       ByteAddress(checkpoint_, offsetof(NativeCheckpoint, recovery)));
  builder_.CreateStore(builder_.getInt32(0),
                       ByteAddress(checkpoint_, offsetof(NativeCheckpoint, instruction)));
  builder_.CreateStore(builder_.getInt8(static_cast<std::uint8_t>(NativeCodeTier::Optimized)),
                       ByteAddress(checkpoint_, offsetof(NativeCheckpoint, tier)));
  builder_.CreateStore(builder_.getInt8(0),
                       ByteAddress(checkpoint_, offsetof(NativeCheckpoint, phase)));
  builder_.CreateStore(checkpoint_,
                       ByteAddress(trace_frame_, offsetof(runtime::NativeTraceFrame, checkpoint)));
}

void FunctionEmitter::StoreCheckpoint() {
  if (checkpoint_ == nullptr) {
    return;
  }
  using runtime::NativeCheckpoint;
  for (const auto reg : roots_.checkpoints.at(instruction_index_)) {
    builder_.CreateStore(Encode(reg),
                         ByteAddress(checkpoint_values_, reg * sizeof(runtime::NativeValue)));
  }
  builder_.CreateStore(builder_.getInt32(static_cast<std::uint32_t>(instruction_index_)),
                       ByteAddress(checkpoint_, offsetof(NativeCheckpoint, instruction)));
  builder_.CreateStore(builder_.getInt8(0),
                       ByteAddress(checkpoint_, offsetof(NativeCheckpoint, phase)));
}

void FunctionEmitter::RecoverIfInvalidated() {
  auto* tier = builder_.CreateLoad(
      builder_.getInt8Ty(), ByteAddress(checkpoint_, offsetof(runtime::NativeCheckpoint, tier)));
  auto* recover = llvm::BasicBlock::Create(module_.getContext(), "deoptimize", body_);
  auto* current = llvm::BasicBlock::Create(module_.getContext(), "current", body_);
  builder_.CreateCondBr(
      builder_.CreateICmpEQ(tier,
                            builder_.getInt8(static_cast<std::uint8_t>(NativeCodeTier::Baseline))),
      recover, current);
  builder_.SetInsertPoint(recover);
  auto helper =
      module_.getOrInsertFunction("vela_native_v4_recover", builder_.getInt8Ty(),
                                  builder_.getPtrTy(), builder_.getPtrTy(), builder_.getPtrTy());
  auto* output = IsResuming() ? llvm::ConstantPointerNull::get(builder_.getPtrTy()) : Output();
  Finish(builder_.CreateCall(helper, {body_->getArg(0), checkpoint_, output}));
  builder_.SetInsertPoint(current);
}

void FunctionEmitter::EmitRecoveryDispatch() {
  auto* checkpoint = body_->getArg(1);
  auto* instruction = builder_.CreateLoad(
      builder_.getInt32Ty(),
      ByteAddress(checkpoint, offsetof(runtime::NativeCheckpoint, instruction)));
  auto* invalid = llvm::BasicBlock::Create(module_.getContext(), "invalid.checkpoint", body_);
  auto* dispatch = builder_.CreateSwitch(instruction, invalid);
  builder_.SetInsertPoint(invalid);
  Finish(Status(builder_, ExecutionStatus::InvalidEntry));
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    if (roots_.safepoints.at(index) == 0) {
      continue;
    }
    dispatch->addCase(builder_.getInt32(static_cast<std::uint32_t>(index)), blocks_.at(index));
  }
}

}  // namespace vela::compiler::native
