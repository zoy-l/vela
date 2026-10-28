#include "compiler/aot/function_emitter.h"

#include <cstddef>
#include <cstdint>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/Support/Alignment.h>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "base/opcode.h"
#include "runtime/native_abi.h"
#include "runtime/native_memory_layout.h"

namespace vela::compiler::aot {

void FunctionEmitter::EmitFrameEntry() {
  using runtime::NativeMemoryLayout;
  using runtime::NativeTraceFrame;
  auto& context = module_.getContext();
  auto* execution = body_->getArg(0);
  auto* pointer = builder_.getPtrTy();
  auto* word = builder_.getInt64Ty();
  auto* initialize = llvm::BasicBlock::Create(context, "initialize", body_);
  auto* failed = llvm::BasicBlock::Create(context, "enter.failed", body_);
  Builder failure(failed);
  auto* error = failure.CreatePHI(builder_.getInt8Ty(), 0);
  failure.CreateRet(error);
  auto check_entry = [&](llvm::Value* status) {
    error->addIncoming(status, builder_.GetInsertBlock());
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(status, Status(builder_, ExecutionStatus::Returned)), initialize,
        failed);
  };
  auto prepare =
      module_.getOrInsertFunction("vela_native_v1_leaf_enter", builder_.getInt8Ty(), pointer);
  llvm::Value* depth = nullptr;
  llvm::Value* depth_address = nullptr;
  llvm::Value* frame_address = nullptr;
  if (leaf_entry_) {
    check_entry(builder_.CreateCall(prepare, {execution}));
  } else {
    auto* valid = llvm::BasicBlock::Create(context, "execution.valid", body_);
    error->addIncoming(Status(builder_, ExecutionStatus::InvalidIsolate),
                       builder_.GetInsertBlock());
    builder_.CreateCondBr(builder_.CreateIsNotNull(execution), valid, failed);
    builder_.SetInsertPoint(valid);
    depth_address = ByteAddress(execution, NativeMemoryLayout::kExecutionDepth);
    frame_address = ByteAddress(execution, NativeMemoryLayout::kExecutionFrame);
    depth = builder_.CreateLoad(word, depth_address);
    auto* available = llvm::BasicBlock::Create(context, "depth.available", body_);
    error->addIncoming(Status(builder_, ExecutionStatus::StackOverflow), builder_.GetInsertBlock());
    builder_.CreateCondBr(builder_.CreateICmpULT(depth, builder_.getInt64(kMaxCallFrames)),
                          available, failed);
    builder_.SetInsertPoint(available);
    auto* first = llvm::BasicBlock::Create(context, "execution.first", body_);
    builder_.CreateCondBr(builder_.CreateICmpEQ(depth, builder_.getInt64(0)), first, initialize);
    builder_.SetInsertPoint(first);
    // 仅最外层入口清理上次调用的异常；嵌套调用只登记逻辑栈和深度。
    check_entry(builder_.CreateCall(prepare, {execution}));
  }
  builder_.SetInsertPoint(initialize);

  auto allocate = [&](std::size_t bytes, std::size_t alignment) {
    Builder allocation(&body_->getEntryBlock(), body_->getEntryBlock().begin());
    auto* storage = allocation.CreateAlloca(llvm::ArrayType::get(builder_.getInt8Ty(), bytes));
    storage->setAlignment(llvm::Align(alignment));
    return storage;
  };
  llvm::Value* resources = nullptr;
  const bool on_stack = frame_bytes_ <= kMaximumNativeStackBytes;
  if (roots_.count != 0 || !on_stack) {
    resources = allocate(sizeof(runtime::NativeFrame), alignof(runtime::NativeFrame));
    auto* access =
        allocate(sizeof(runtime::NativeFrameAccess), alignof(runtime::NativeFrameAccess));
    llvm::Value* stack_roots = llvm::ConstantPointerNull::get(pointer);
    if (on_stack && roots_.count != 0) {
      stack_roots = allocate(roots_.count * sizeof(runtime::Value), alignof(runtime::Value));
    }
    auto storage = module_.getOrInsertFunction("vela_native_v1_frame_storage", builder_.getInt8Ty(),
                                               pointer, word, word, pointer, pointer, pointer);
    auto* entered = builder_.CreateCall(storage, {execution, builder_.getInt64(roots_.count),
                                                  builder_.getInt64(on_stack ? 0 : storage_bytes_),
                                                  stack_roots, resources, access});
    auto* allocated = llvm::BasicBlock::Create(context, "storage.ready", body_);
    error->addIncoming(entered, builder_.GetInsertBlock());
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(entered, Status(builder_, ExecutionStatus::Returned)), allocated,
        failed);
    builder_.SetInsertPoint(allocated);
    root_slots_ =
        builder_.CreateLoad(pointer, ByteAddress(access, NativeMemoryLayout::kFrameRoots));
    frame_storage_ =
        builder_.CreateLoad(pointer, ByteAddress(access, NativeMemoryLayout::kFrameStorage));
  }
  cleanup_ = llvm::BasicBlock::Create(context, "cleanup", body_);
  Builder cleanup(cleanup_);
  status_ = cleanup.CreatePHI(builder_.getInt8Ty(), 0, "status");
  if (!leaf_entry_) {
    auto* frame = allocate(sizeof(NativeTraceFrame), alignof(NativeTraceFrame));
    auto* previous = builder_.CreateLoad(pointer, frame_address);
    builder_.CreateStore(previous, ByteAddress(frame, offsetof(NativeTraceFrame, previous)));
    const auto position = offsetof(NativeTraceFrame, position);
    builder_.CreateStore(builder_.getInt32(static_cast<std::uint32_t>(function_index_)),
                         ByteAddress(frame, position + offsetof(runtime::TracePosition, function)));
    instruction_slot_ =
        ByteAddress(frame, position + offsetof(runtime::TracePosition, instruction));
    builder_.CreateStore(builder_.getInt32(0), instruction_slot_);
    builder_.CreateStore(frame, frame_address);
    builder_.CreateStore(builder_.CreateAdd(depth, builder_.getInt64(1)), depth_address);
    cleanup.CreateStore(previous, frame_address);
    cleanup.CreateStore(depth, depth_address);
  }
  if (resources != nullptr) {
    auto release =
        module_.getOrInsertFunction("vela_native_v1_frame_release", cleanup.getVoidTy(), pointer);
    cleanup.CreateCall(release, {resources});
  }
  cleanup.CreateRet(status_);
}

void FunctionEmitter::CacheImports() {
  auto lookup = module_.getOrInsertFunction("vela_native_v1_import", builder_.getPtrTy(),
                                            builder_.getPtrTy(), builder_.getInt32Ty());
  for (const auto& instruction : source_.instructions) {
    if (instruction.opcode != Opcode::Call ||
        program_.functions.at(instruction.index).native_symbol.empty() ||
        imports_.contains(instruction.index)) {
      continue;
    }
    // 绑定随实例创建后不可变；闭包临时切换实例会在返回时恢复，作用域内可复用。
    auto* slot =
        builder_.CreateCall(lookup, {body_->getArg(0), builder_.getInt32(instruction.index)});
    imports_.emplace(instruction.index, builder_.CreateLoad(builder_.getPtrTy(), slot));
  }
}

}  // namespace vela::compiler::aot
