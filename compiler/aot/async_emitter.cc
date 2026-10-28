#include "compiler/aot/function_emitter.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include "base/async_operation.h"
#include "base/execution_status.h"
#include "base/opcode.h"
#include "compiler/aot/native_values.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::aot {

llvm::Value* FunctionEmitter::Output() {
  return body_->getArg(static_cast<unsigned>(body_->arg_size() - 1));
}

void FunctionEmitter::StoreAsyncResult() {
  auto* result = builder_.CreateLoad(NativeRecordType(module_.getContext()), result_);
  builder_.CreateStore(DecodeValue(builder_, result, source_.return_type), Output());
  Finish(Status(builder_, ExecutionStatus::Returned));
}

void FunctionEmitter::EmitAsyncFailure(llvm::Value* status) {
  auto pending = module_.getOrInsertFunction("vela_native_v1_has_exception", builder_.getInt8Ty(),
                                             builder_.getPtrTy());
  auto* has_exception = builder_.CreateCall(pending, {body_->getArg(0)});
  auto* complete = llvm::BasicBlock::Create(module_.getContext(), "async.error", body_);
  auto* abort = llvm::BasicBlock::Create(module_.getContext(), "async.abort", body_);
  builder_.CreateCondBr(builder_.CreateICmpNE(has_exception, builder_.getInt8(0)), complete, abort);
  builder_.SetInsertPoint(abort);
  Finish(status);
  builder_.SetInsertPoint(complete);
  auto helper =
      module_.getOrInsertFunction("vela_native_v1_async_error", builder_.getInt8Ty(),
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
  auto helper = module_.getOrInsertFunction("vela_native_v1_async_return", builder_.getInt8Ty(),
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
  const auto& saved = roots_.suspended.at(instruction_index_);
  for (std::size_t slot = 0; slot < saved.size(); ++slot) {
    Pack(Encode(saved.at(slot)), slot);
  }
  auto* source = Pack(Encode(instruction.lhs), saved.size());
  auto helper = module_.getOrInsertFunction(
      "vela_native_v1_suspend", builder_.getInt8Ty(), builder_.getPtrTy(), builder_.getInt32Ty(),
      builder_.getPtrTy(), builder_.getInt32Ty(), builder_.getPtrTy(), builder_.getPtrTy(),
      builder_.getInt64Ty(), builder_.getPtrTy());
  auto* status = builder_.CreateCall(
      helper, {
                  body_->getArg(0),
                  builder_.getInt32(static_cast<std::uint32_t>(function_index_)),
                  generated_.subspan(function_index_).front().resume,
                  builder_.getInt32(static_cast<std::uint32_t>(instruction_index_)),
                  source,
                  arguments_,
                  builder_.getInt64(saved.size()),
                  result_,
              });
  CheckStatus(status);
  StoreAsyncResult();
}

void FunctionEmitter::EmitResumeDispatch() {
  auto* state = body_->getArg(1);
  auto resume = module_.getOrInsertFunction("vela_native_v1_resume_instruction",
                                            builder_.getInt32Ty(), builder_.getPtrTy());
  auto* instruction = builder_.CreateCall(resume, {state});
  auto* invalid = llvm::BasicBlock::Create(module_.getContext(), "invalid.resume", body_);
  auto* dispatch = builder_.CreateSwitch(instruction, invalid);
  builder_.SetInsertPoint(invalid);
  Finish(Status(builder_, ExecutionStatus::InvalidEntry));
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    const auto& source = source_.instructions.at(index);
    if (source.opcode != Opcode::Await) {
      continue;
    }
    instruction_index_ = index;
    auto* restore = llvm::BasicBlock::Create(module_.getContext(), "resume", body_);
    dispatch->addCase(builder_.getInt32(static_cast<std::uint32_t>(index)), restore);
    builder_.SetInsertPoint(restore);
    SetPosition();
    const auto& saved = roots_.suspended.at(index);
    auto read = module_.getOrInsertFunction("vela_native_v1_resume_value", builder_.getVoidTy(),
                                            builder_.getPtrTy(), builder_.getInt64Ty(),
                                            builder_.getPtrTy());
    for (std::size_t slot = 0; slot < saved.size(); ++slot) {
      const auto reg = saved.at(slot);
      builder_.CreateCall(read, {state, builder_.getInt64(slot), result_});
      Unpack(reg);
      if (const auto root_slot = roots_.slots.at(reg)) {
        WriteHeapValue(RootAddress(*root_slot), reg);
      }
    }
    // 活动引用先进入当前帧根，再清空挂起快照；错误恢复也可能触发 GC。
    auto release = module_.getOrInsertFunction("vela_native_v1_resume_release",
                                               builder_.getVoidTy(), builder_.getPtrTy());
    builder_.CreateCall(release, {state});
    auto outcome =
        module_.getOrInsertFunction("vela_native_v1_resume_outcome", builder_.getInt8Ty(),
                                    builder_.getPtrTy(), builder_.getPtrTy(), builder_.getPtrTy());
    CheckStatus(builder_.CreateCall(outcome, {body_->getArg(0), state, result_}));
    Unpack(source.dst);
    builder_.CreateBr(blocks_.at(index + 1));
  }
}

std::expected<void, std::string> FunctionEmitter::EmitAsyncInstruction(
    const ir::Instruction& instruction) {
  auto* context = body_->getArg(0);
  auto* pointer = builder_.getPtrTy();
  auto* word = builder_.getInt32Ty();
  auto* status_type = builder_.getInt8Ty();
  auto* const index = builder_.getInt32(instruction.index);
  llvm::Value* status = nullptr;
  switch (instruction.opcode) {
    case Opcode::NewFuture: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_new_future", status_type, pointer,
                                                pointer, status_type, pointer);
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
      for (std::size_t argument = 0; argument < AsyncArity(operation); ++argument) {
        Pack(Encode(static_cast<ir::RegisterId>(instruction.lhs + argument)), argument);
      }
      auto helper = module_.getOrInsertFunction("vela_native_v1_async_call", status_type, pointer,
                                                word, pointer, pointer);
      status = builder_.CreateCall(helper, {context, index, arguments_, result_});
      break;
    }
    case Opcode::SleepFuture: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_sleep", status_type, pointer,
                                                builder_.getInt64Ty(), pointer);
      status = builder_.CreateCall(helper, {context, Payload(instruction.lhs), result_});
      break;
    }
    default:
      std::unreachable();
  }
  CheckStatus(status);
  Unpack(instruction.dst);
  return {};
}

}  // namespace vela::compiler::aot
