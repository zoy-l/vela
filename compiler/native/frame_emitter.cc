#include "compiler/native/function_emitter.h"

#include <cstddef>
#include <cstdint>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Support/Alignment.h>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "base/opcode.h"
#include "runtime/native_abi.h"
#include "runtime/native_memory_layout.h"

namespace vela::compiler::native {

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
  auto* valid = llvm::BasicBlock::Create(context, "execution.valid", body_);
  error->addIncoming(Status(builder_, ExecutionStatus::InvalidIsolate), builder_.GetInsertBlock());
  builder_.CreateCondBr(builder_.CreateIsNotNull(execution), valid, failed,
                        llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(valid);
  auto* depth_address = ByteAddress(execution, NativeMemoryLayout::kExecutionDepth);
  auto* frame_address = ByteAddress(execution, NativeMemoryLayout::kExecutionFrame);
  auto* depth = builder_.CreateLoad(word, depth_address);
  auto* available = llvm::BasicBlock::Create(context, "depth.available", body_);
  error->addIncoming(Status(builder_, ExecutionStatus::StackOverflow), builder_.GetInsertBlock());
  builder_.CreateCondBr(builder_.CreateICmpULT(depth, builder_.getInt64(kMaxCallFrames)), available,
                        failed, llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(available);
  auto* first = llvm::BasicBlock::Create(context, "execution.first", body_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(depth, builder_.getInt64(0)), first, initialize);
  builder_.SetInsertPoint(first);
  // 与 NativeExecution::PrepareEntry 共用布局事实；成功入口不跨 helper 调用边界。
  // 仅最外层清理上次异常，嵌套调用必须保留 catch 作用域中的异常状态。
  builder_.CreateStore(llvm::Constant::getNullValue(NativeRecordType(context)),
                       ByteAddress(execution, NativeMemoryLayout::kExecutionException));
  builder_.CreateStore(
      builder_.getInt8(0),
      ByteAddress(execution, NativeMemoryLayout::kExecutionExceptionAllocationFailed));
  builder_.CreateBr(initialize);
  builder_.SetInsertPoint(initialize);

  auto allocate = [&](std::size_t bytes, std::size_t alignment) {
    Builder allocation(&body_->getEntryBlock(), body_->getEntryBlock().begin());
    auto* storage = allocation.CreateAlloca(llvm::ArrayType::get(builder_.getInt8Ty(), bytes));
    storage->setAlignment(llvm::Align(alignment));
    return storage;
  };
  llvm::Value* resources = nullptr;
  llvm::Value* root_range = nullptr;
  const bool on_stack = frame_bytes_ <= kMaximumNativeStackBytes;
  if (leaf_entry_ && roots_.count != 0) {
    // 成功路径没有 GC 安全点；根槽只在失败分支写入并由 leaf_failure 临时登记。
    root_slots_ = allocate(roots_.count * sizeof(runtime::Value), alignof(runtime::Value));
  } else if (on_stack && roots_.count != 0) {
    // 栈上根只借用 RootRange，不构造拥有堆存储的 NativeFrame。
    root_slots_ = allocate(roots_.count * sizeof(runtime::Value), alignof(runtime::Value));
    root_range = allocate(sizeof(runtime::RootRange), alignof(runtime::RootRange));
    auto enter = module_.getOrInsertFunction("vela_native_v1_roots_enter", builder_.getVoidTy(),
                                             pointer, pointer, word, pointer);
    builder_.CreateCall(enter,
                        {execution, root_slots_, builder_.getInt64(roots_.count), root_range});
  } else if (!on_stack) {
    resources = allocate(sizeof(runtime::NativeFrame), alignof(runtime::NativeFrame));
    auto* access =
        allocate(sizeof(runtime::NativeFrameAccess), alignof(runtime::NativeFrameAccess));
    auto storage = module_.getOrInsertFunction("vela_native_v1_frame_storage", builder_.getInt8Ty(),
                                               pointer, word, word, pointer, pointer);
    auto* entered =
        builder_.CreateCall(storage, {execution, builder_.getInt64(roots_.count),
                                      builder_.getInt64(storage_bytes_), resources, access});
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
    builder_.CreateStore(image_, ByteAddress(frame, offsetof(NativeTraceFrame, image)));
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
  if (root_range != nullptr) {
    auto exit =
        module_.getOrInsertFunction("vela_native_v1_roots_exit", cleanup.getVoidTy(), pointer);
    cleanup.CreateCall(exit, {root_range});
  } else if (resources != nullptr) {
    auto release =
        module_.getOrInsertFunction("vela_native_v1_frame_release", cleanup.getVoidTy(), pointer);
    cleanup.CreateCall(release, {resources});
  }
  cleanup.CreateRet(status_);
}

void FunctionEmitter::CacheImports() {
  for (const auto& instruction : source_.instructions) {
    if (instruction.opcode != Opcode::Call ||
        program_.functions.at(instruction.index).native_symbol.empty() ||
        imports_.contains(instruction.index)) {
      continue;
    }
    // 绑定随实例创建后不可变；闭包临时切换实例会在返回时恢复，作用域内可复用。
    imports_.emplace(instruction.index,
                     LoadImport(builder_, module_, body_->getArg(0), instruction.index));
  }
}

}  // namespace vela::compiler::native
