#include "base/native_program_mode.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/function_emitter.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Value.h>

#include "compiler/native/native_values.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_deoptimization.h"
#include "runtime/native/native_memory_layout.h"

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
    case NativeEntryKind::Borrowed:
      return function.borrowed;
    case NativeEntryKind::Leaf:
      return function.leaf;
  }
  std::unreachable();
}

bool FunctionEmitter::IsRecovering() const noexcept {
  return kind_ == NativeEntryKind::RecoverInitial || kind_ == NativeEntryKind::RecoverResume;
}

bool FunctionEmitter::IsResuming() const noexcept {
  return kind_ == NativeEntryKind::Resume || kind_ == NativeEntryKind::RecoverResume;
}

bool FunctionEmitter::HasRecovery() const {
  return generated_.subspan(function_index_).front().HasRecovery(tier_);
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
  if (!HasRecovery() || kind_ != NativeEntryKind::Resume) {
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
  const auto& function = generated_.subspan(function_index_).front();
  auto* entry = IsResuming() ? function.resume_recovery : function.recovery;
  auto* word = builder_.getInt64Ty();
  auto* type = llvm::StructType::get(module_.getContext(), {builder_.getPtrTy(), word, word, word});
  auto* description =
      llvm::ConstantStruct::get(type, {
                                          entry,
                                          builder_.getInt64(roots_.count),
                                          builder_.getInt64(source_.register_count),
                                          builder_.getInt64(escape_.reference_count),
                                      });
  auto* layout = new llvm::GlobalVariable(module_, type, true, llvm::GlobalValue::PrivateLinkage,
                                          description, body_->getName() + "_checkpoint_layout");
  checkpoint_layout_ = layout;
  Builder allocation(&body_->getEntryBlock(), body_->getEntryBlock().begin());
  checkpoint_initialized_ = allocation.CreateAlloca(builder_.getInt1Ty());
  builder_.CreateStore(builder_.getFalse(), checkpoint_initialized_);
  builder_.CreateStore(builder_.getInt8(static_cast<std::uint8_t>(NativeCodeTier::Optimized)),
                       ByteAddress(checkpoint_, offsetof(runtime::NativeCheckpoint, tier)));
}

void FunctionEmitter::StoreRecoveryDescription() {
  using runtime::NativeCheckpoint;
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
  builder_.CreateStore(checkpoint_layout_,
                       ByteAddress(checkpoint_, offsetof(NativeCheckpoint, layout)));
}

void FunctionEmitter::PrepareCheckpoint() {
  auto& context = module_.getContext();
  auto* initialize = llvm::BasicBlock::Create(context, "checkpoint.initialize", body_);
  auto* ready = llvm::BasicBlock::Create(context, "checkpoint.ready", body_);
  builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), checkpoint_initialized_), ready,
                        initialize);
  builder_.SetInsertPoint(initialize);
  // 只有会跨可观察边界的路径才需要链接检查点。纯返回分支不发布检查点；后续调用仍复用
  // 已初始化的数据，不能覆盖被嵌套调用或发布请求修改的 tier。
  using runtime::NativeCheckpoint;
  using runtime::NativeVirtualState;
  auto* null_pointer = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  builder_.CreateStore(virtual_state_ == nullptr ? null_pointer : virtual_state_,
                       ByteAddress(checkpoint_, offsetof(NativeCheckpoint, materialization)));
  if (virtual_state_ != nullptr) {
    // 被调用方安全点会物化本帧的虚拟对象，因此恢复描述须在首次链接时就完整。
    StoreRecoveryDescription();
    builder_.CreateStore(null_pointer,
                         ByteAddress(virtual_state_, offsetof(NativeVirtualState, graph)));
    builder_.CreateStore(virtual_references_,
                         ByteAddress(virtual_state_, offsetof(NativeVirtualState, references)));
    auto* instance = builder_.CreateLoad(
        builder_.getPtrTy(),
        ByteAddress(body_->getArg(0), runtime::NativeMemoryLayout::kExecutionInstance));
    auto* program = builder_.CreateLoad(
        builder_.getPtrTy(), ByteAddress(instance, runtime::NativeMemoryLayout::kInstanceAnchor +
                                                       runtime::NativeMemoryLayout::kValuePayload));
    builder_.CreateStore(program,
                         ByteAddress(virtual_state_, offsetof(NativeVirtualState, program)));
    for (std::size_t storage = 0; storage < escape_.allocations.size(); ++storage) {
      const auto& allocation = escape_.allocations.at(storage);
      if (allocation.decision == AllocationDecision::ScalarReplacement) {
        builder_.CreateStore(
            slots_.at(allocation.reference),
            builder_.CreateConstGEP1_64(builder_.getPtrTy(), virtual_references_, storage));
      }
    }
  }
  builder_.CreateStore(checkpoint_,
                       ByteAddress(trace_frame_, offsetof(runtime::NativeTraceFrame, checkpoint)));
  builder_.CreateStore(builder_.getTrue(), checkpoint_initialized_);
  builder_.CreateBr(ready);
  builder_.SetInsertPoint(ready);
}

void FunctionEmitter::StoreCheckpoint() {
  if (checkpoint_ == nullptr) {
    return;
  }
  PrepareCheckpoint();
  if (virtual_state_ == nullptr) {
    StoreRecoveryDescription();
  }
  using runtime::NativeCheckpoint;
  const auto origin = origins_.subspan(instruction_index_).front();
  const auto& values = escape_.checkpoints.empty() ? roots_.checkpoints.at(instruction_index_)
                                                   : escape_.checkpoints.at(origin).required;
  for (const auto reg : values) {
    auto* value = Encode(reg);
    const auto& type = source_.value_types.at(reg);
    // 无物化图时只有类型化的基线恢复会读取这些槽；非空标量的种类由原函数确定，
    // 不必在每个调用点重复保存标签。动态值、可空值及物化输入仍保存完整 NativeValue。
    if (virtual_state_ == nullptr && IsScalar(type) && !HasValidity(type)) {
      value = builder_.CreateExtractValue(value, 0);
    }
    builder_.CreateStore(value,
                         ByteAddress(checkpoint_values_, reg * sizeof(runtime::NativeValue)));
  }
  if (!escape_.checkpoints.empty()) {
    for (const auto& alias : escape_.checkpoints.at(origin).aliases) {
      const auto reference = escape_.allocations.at(alias.allocation).reference;
      builder_.CreateStore(
          Encode(reference),
          ByteAddress(checkpoint_values_, alias.reg * sizeof(runtime::NativeValue)));
    }
    builder_.CreateStore(VirtualGraph(origin),
                         ByteAddress(virtual_state_, offsetof(runtime::NativeVirtualState, graph)));
  }
  builder_.CreateStore(builder_.getInt32(static_cast<std::uint32_t>(origin)),
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
  Recover();
  builder_.SetInsertPoint(current);
}

void FunctionEmitter::Recover() {
  auto helper =
      module_.getOrInsertFunction("vela_native_v4_recover", builder_.getInt8Ty(),
                                  builder_.getPtrTy(), builder_.getPtrTy(), builder_.getPtrTy());
  auto* output = IsResuming() ? llvm::ConstantPointerNull::get(builder_.getPtrTy()) : Output();
  Finish(builder_.CreateCall(helper, {body_->getArg(0), checkpoint_, output}));
}

void FunctionEmitter::EmitRecoveryDispatch() {
  auto* checkpoint = body_->getArg(1);
  auto* instruction = builder_.CreateLoad(
      builder_.getInt32Ty(),
      ByteAddress(checkpoint, offsetof(runtime::NativeCheckpoint, instruction)));
  auto* dispatch = ResumeDispatch(instruction);
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    if (roots_.safepoints.at(index) == 0) {
      continue;
    }
    dispatch->addCase(builder_.getInt32(static_cast<std::uint32_t>(index)), blocks_.at(index));
  }
}

}  // namespace vela::compiler::native
