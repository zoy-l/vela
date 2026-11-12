#include "compiler/native/function_emitter.h"
#include "runtime/native/native_symbols.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include "base/async_operation.h"
#include "base/execution_status.h"
#include "base/opcode.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_async.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {

llvm::Value* FunctionEmitter::Output() {
  return body_->getArg(static_cast<unsigned>(body_->arg_size() - 1));
}

void FunctionEmitter::StoreAsyncResult() {
  auto* result = builder_.CreateLoad(NativeRecordType(module_.getContext()), result_);
  builder_.CreateStore(DecodeValue(builder_, result, source_.return_type), Output());
  Finish(Status(builder_, ExecutionStatus::Returned));
}

void FunctionEmitter::EmitAsyncFailure(llvm::Value* status) {
  if (IsResuming()) {
    // 激活已持有结果 Future，恢复失败由该激活直接完成，无需分配错误 Future。
    Finish(status);
    return;
  }
  auto pending = module_.getOrInsertFunction(runtime::kNativeHasExceptionSymbol,
                                             builder_.getInt8Ty(), builder_.getPtrTy());
  auto* has_exception = builder_.CreateCall(pending, {body_->getArg(0)});
  auto* complete = llvm::BasicBlock::Create(module_.getContext(), "async.error", body_);
  auto* abort = llvm::BasicBlock::Create(module_.getContext(), "async.abort", body_);
  builder_.CreateCondBr(builder_.CreateICmpNE(has_exception, builder_.getInt8(0)), complete, abort);
  builder_.SetInsertPoint(abort);
  Finish(status);
  builder_.SetInsertPoint(complete);
  auto helper =
      module_.getOrInsertFunction(runtime::kNativeAsyncErrorSymbol, builder_.getInt8Ty(),
                                  builder_.getPtrTy(), builder_.getInt32Ty(), builder_.getPtrTy());
  auto* completed = builder_.CreateCall(
      helper,
      {body_->getArg(0), builder_.getInt32(static_cast<std::uint32_t>(function_index_)), result_});
  auto* returned = llvm::BasicBlock::Create(module_.getContext(), "async.failed.future", body_);
  auto* failed =
      llvm::BasicBlock::Create(module_.getContext(), "async.error.allocation.failed", body_);
  builder_.CreateCondBr(
      builder_.CreateICmpEQ(completed, Status(builder_, ExecutionStatus::Returned)), returned,
      failed);
  builder_.SetInsertPoint(failed);
  Finish(completed);
  builder_.SetInsertPoint(returned);
  StoreAsyncResult();
}

void FunctionEmitter::EmitAsyncReturn(const ir::Instruction& instruction) {
  if (IsResuming()) {
    auto helper = module_.getOrInsertFunction(
        runtime::kNativeResumeReturnSymbol, builder_.getInt8Ty(), builder_.getPtrTy(),
        builder_.getPtrTy(), builder_.getInt32Ty(), builder_.getPtrTy());
    auto* status = builder_.CreateCall(
        helper, {
                    body_->getArg(0),
                    ResumeState(),
                    builder_.getInt32(static_cast<std::uint32_t>(function_index_)),
                    Pack(Encode(instruction.dst), 0),
                });
    CheckStatus(status);
    Finish(Status(builder_, ExecutionStatus::Returned));
    return;
  }
  auto helper = module_.getOrInsertFunction(runtime::kNativeAsyncReturnSymbol, builder_.getInt8Ty(),
                                            builder_.getPtrTy(), builder_.getInt32Ty(),
                                            builder_.getPtrTy(), builder_.getPtrTy());
  auto* payload = Pack(Encode(instruction.dst), 0);
  auto* status = builder_.CreateCall(
      helper, {
                  body_->getArg(0),
                  builder_.getInt32(static_cast<std::uint32_t>(function_index_)),
                  payload,
                  result_,
              });
  CheckStatus(status);
  StoreAsyncResult();
}

void FunctionEmitter::EmitAwait(const ir::Instruction& instruction) {
  auto* function = builder_.getInt32(static_cast<std::uint32_t>(function_index_));
  auto* origin = builder_.getInt32(static_cast<std::uint32_t>(origins_[instruction_index_]));
  const auto& saved = roots_.suspended.at(instruction_index_);
  for (std::size_t slot = 0; slot < saved.size(); ++slot) {
    Pack(Encode(saved.at(slot)), slot);
  }
  auto* source = Pack(Encode(instruction.lhs), saved.size());
  if (IsResuming()) {
    auto helper = module_.getOrInsertFunction(
        runtime::kNativeResuspendSymbol, builder_.getVoidTy(), builder_.getPtrTy(),
        builder_.getInt32Ty(), builder_.getPtrTy(), builder_.getPtrTy(), builder_.getInt64Ty());
    builder_.CreateCall(helper, {
                                    ResumeState(),
                                    origin,
                                    source,
                                    arguments_,
                                    builder_.getInt64(saved.size()),
                                });
    Finish(Status(builder_, ExecutionStatus::Returned));
    return;
  }
  const auto next = instruction_index_ + 1;
  if (IsClosedLinkedProgram(program_.native_mode, placement_) && source_.handlers.empty() &&
      roots_.bounded_polls && saved.empty() && IsScalar(source_.value_types[instruction.lhs]) &&
      next < source_.instructions.size() && source_.instructions[next].opcode == Opcode::Return &&
      source_.instructions[next].dst == instruction.dst) {
    auto helper = module_.getOrInsertFunction(
        runtime::kNativeScalarAwaitReturnSymbol, builder_.getInt8Ty(), builder_.getPtrTy(),
        builder_.getInt32Ty(), builder_.getPtrTy(), builder_.getInt32Ty(), builder_.getPtrTy(),
        builder_.getPtrTy());
    CheckStatus(builder_.CreateCall(helper, {
                                                body_->getArg(0),
                                                function,
                                                generated_[function_index_].resume,
                                                origin,
                                                source,
                                                result_,
                                            }));
    StoreAsyncResult();
    return;
  }
  auto helper = module_.getOrInsertFunction(
      runtime::kNativeSuspendSymbol, builder_.getInt8Ty(), builder_.getPtrTy(),
      builder_.getInt32Ty(), builder_.getPtrTy(), builder_.getInt32Ty(), builder_.getPtrTy(),
      builder_.getPtrTy(), builder_.getInt64Ty(), builder_.getInt64Ty(), builder_.getPtrTy());
  auto* status = builder_.CreateCall(helper, {
                                                 body_->getArg(0),
                                                 function,
                                                 generated_[function_index_].resume,
                                                 origin,
                                                 source,
                                                 arguments_,
                                                 builder_.getInt64(saved.size()),
                                                 builder_.getInt64(suspension_capacity_),
                                                 result_,
                                             });
  CheckStatus(status);
  StoreAsyncResult();
}

void FunctionEmitter::EmitResumeDispatch() {
  auto* state = ResumeState();
  auto* instruction = builder_.CreateLoad(
      builder_.getInt32Ty(), ByteAddress(state, offsetof(runtime::NativeResumeState, instruction)));
  auto* saved_values = builder_.CreateLoad(
      builder_.getPtrTy(), ByteAddress(state, offsetof(runtime::NativeResumeState, saved)));
  auto* dispatch = ResumeDispatch(instruction);
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    const auto& source = source_.instructions.at(index);
    if (source.opcode != Opcode::Await) {
      continue;
    }
    instruction_index_ = index;
    auto* restore = llvm::BasicBlock::Create(module_.getContext(), "resume", body_);
    dispatch->addCase(builder_.getInt32(static_cast<std::uint32_t>(origins_[index])), restore);
    builder_.SetInsertPoint(restore);
    SetPosition();
    for (const auto position : roots_.rematerialized.at(index)) {
      // 仅单一定义的立即数会进入此列表，不需要保存、分配或错误分支。
      EmitConstant(source_.instructions.at(position));
    }
    const auto& saved = roots_.suspended.at(index);
    for (std::size_t slot = 0; slot < saved.size(); ++slot) {
      const auto reg = saved.at(slot);
      const auto& type = source_.value_types.at(reg);
      auto* value = builder_.CreateLoad(NativeRecordType(module_.getContext()),
                                        ByteAddress(saved_values, slot * sizeof(runtime::Value)));
      Store(reg, DecodeValue(builder_, value, type), type);
      if (const auto root_slot = roots_.slots.find(reg); root_slot != roots_.slots.end()) {
        StoreRootValue(reg, RootAddress(root_slot->second));
      }
    }
    // 活动引用先进入当前帧根，再清空挂起快照；错误恢复也可能触发 GC。
    auto outcome =
        module_.getOrInsertFunction(runtime::kNativeResumeOutcomeSymbol, builder_.getInt8Ty(),
                                    builder_.getPtrTy(), builder_.getPtrTy(), builder_.getPtrTy());
    CheckStatus(builder_.CreateCall(outcome, {body_->getArg(0), state, result_}));
    Unpack(source.dst);
    builder_.CreateBr(blocks_.at(index + 1));
  }
}

void FunctionEmitter::EmitAsyncInstruction(const ir::Instruction& instruction) {
  auto* context = body_->getArg(0);
  auto* pointer = builder_.getPtrTy();
  auto* word = builder_.getInt32Ty();
  auto* status_type = builder_.getInt8Ty();
  auto* const index = builder_.getInt32(instruction.index);
  llvm::Value* status = nullptr;
  switch (instruction.opcode) {
    case Opcode::NewFuture: {
      auto helper = module_.getOrInsertFunction(runtime::kNativeNewFutureSymbol, status_type,
                                                pointer, pointer, status_type, pointer);
      status = builder_.CreateCall(helper,
                                   {
                                       context,
                                       Pack(Encode(instruction.lhs), 0),
                                       builder_.getInt8(static_cast<std::uint8_t>(instruction.rhs)),
                                       result_,
                                   });
      break;
    }
    case Opcode::AsyncCall: {
      const auto operation = static_cast<AsyncOperation>(instruction.index);
      PackArguments(instruction.lhs, AsyncArity(operation));
      auto helper = module_.getOrInsertFunction(runtime::kNativeAsyncCallSymbol, status_type,
                                                pointer, word, pointer, pointer);
      status = builder_.CreateCall(helper, {context, index, arguments_, result_});
      break;
    }
    case Opcode::SleepFuture: {
      auto helper = module_.getOrInsertFunction(runtime::kNativeSleepSymbol, status_type, pointer,
                                                builder_.getInt64Ty(), pointer);
      status = builder_.CreateCall(helper, {context, Payload(instruction.lhs), result_});
      break;
    }
    default:
      std::unreachable();
  }
  CompleteCall(status, instruction.dst);
}

}  // namespace vela::compiler::native
