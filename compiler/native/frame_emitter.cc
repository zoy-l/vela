#include "compiler/native/function_emitter.h"
#include "runtime/native/native_symbols.h"

#include <absl/container/flat_hash_map.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Alignment.h>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/ir/ir.h"
#include "compiler/native/field_access.h"
#include "compiler/native/native_values.h"
#include "runtime/heap/heap.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {

llvm::Value* FunctionEmitter::HasStackBudget(llvm::Value* depth) {
  using runtime::NativeMemoryLayout;
  auto* word = builder_.getInt64Ty();
  auto* stack_limit = builder_.CreateLoad(
      word, ByteAddress(body_->getArg(0), NativeMemoryLayout::kExecutionStackLimit));
  stack_limit->setMetadata(llvm::LLVMContext::MD_invariant_load,
                           llvm::MDNode::get(module_.getContext(), {}));
  auto* stack_position = builder_.CreateIntrinsic(llvm::Intrinsic::frameaddress,
                                                  {builder_.getPtrTy()}, {builder_.getInt32(0)});
  auto* stack_available =
      builder_.CreateICmpUGT(builder_.CreatePtrToInt(stack_position, word), stack_limit);
  return builder_.CreateAnd(builder_.CreateICmpULT(depth, builder_.getInt64(kMaxCallFrames)),
                            stack_available);
}

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
  if (kind_ == NativeEntryKind::Leaf) {
    failure.CreateStore(
        failure.getInt64(runtime::EncodeNativeTracePosition(
            {.function = static_cast<std::uint32_t>(function_index_), .instruction = 0})),
        body_->getArg(static_cast<unsigned>(body_->arg_size() - 1)));
  }
  failure.CreateRet(error);
  // execution 及固定栈状态由真实入口验证；每次内部调用仍检查动态栈预算。
  auto* depth_address = ByteAddress(execution, NativeMemoryLayout::kExecutionDepth);
  auto* depth = builder_.CreateLoad(word, depth_address);
  entry_depth_ = depth;
  auto* available = llvm::BasicBlock::Create(context, "depth.available", body_);
  error->addIncoming(Status(builder_, ExecutionStatus::StackOverflow), builder_.GetInsertBlock());
  builder_.CreateCondBr(HasStackBudget(depth), available, failed,
                        llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(available);
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
  if (IsRecovering()) {
    // 优化帧已预留基线存储，并持有根区间直到恢复返回；此处不分配资源或重复登记根。
  } else if (leaf_entry_ && roots_.count != 0) {
    // 成功路径没有 GC 安全点；根槽只在失败分支写入并由 leaf_failure 临时登记。
    root_slots_ = allocate(roots_.count * sizeof(runtime::Value), alignof(runtime::Value));
  } else if (on_stack && roots_.count != 0) {
    // 栈上根只借用 RootRange，不构造拥有堆存储的 NativeFrame。
    root_slots_ = allocate(roots_.count * sizeof(runtime::Value), alignof(runtime::Value));
    root_range = allocate(sizeof(runtime::RootRange), alignof(runtime::RootRange));
    auto enter = module_.getOrInsertFunction(runtime::kNativeRootsEnterSymbol, builder_.getVoidTy(),
                                             pointer, pointer, word, pointer);
    builder_.CreateCall(enter,
                        {execution, root_slots_, builder_.getInt64(roots_.count), root_range});
  } else if (!on_stack) {
    resources = allocate(sizeof(runtime::NativeFrame), alignof(runtime::NativeFrame));
    auto* access =
        allocate(sizeof(runtime::NativeFrameAccess), alignof(runtime::NativeFrameAccess));
    auto storage =
        module_.getOrInsertFunction(runtime::kNativeFrameStorageSymbol, builder_.getInt8Ty(),
                                    pointer, word, word, pointer, pointer);
    auto* entered = builder_.CreateCall(storage, {
                                                     execution,
                                                     builder_.getInt64(roots_.count),
                                                     builder_.getInt64(storage_bytes_),
                                                     resources,
                                                     access,
                                                 });
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
  if (construction_depth_ != nullptr) {
    auto exit = module_.getOrInsertFunction(runtime::kNativeInitializationExitSymbol,
                                            cleanup.getVoidTy(), pointer, cleanup.getInt32Ty());
    cleanup.CreateCall(exit,
                       {execution, cleanup.CreateLoad(cleanup.getInt32Ty(), construction_depth_)});
  }
  if (!leaf_entry_) {
    auto* frame = allocate(sizeof(NativeTraceFrame), alignof(NativeTraceFrame));
    trace_frame_ = frame;
    Builder allocation(&body_->getEntryBlock(), body_->getEntryBlock().begin());
    trace_initialized_ = allocation.CreateAlloca(builder_.getInt1Ty());
    builder_.CreateStore(builder_.getFalse(), trace_initialized_);
    instruction_slot_ = ByteAddress(frame, offsetof(NativeTraceFrame, position) +
                                               offsetof(runtime::TracePosition, instruction));
    auto* restore = llvm::BasicBlock::Create(context, "frame.restore", body_);
    auto* restored = llvm::BasicBlock::Create(context, "frame.restored", body_);
    cleanup.CreateCondBr(cleanup.CreateLoad(cleanup.getInt1Ty(), trace_initialized_), restore,
                         restored);
    cleanup.SetInsertPoint(restore);
    auto* previous = cleanup.CreateLoad(
        pointer, cleanup.CreateConstGEP1_64(cleanup.getInt8Ty(), frame,
                                            offsetof(NativeTraceFrame, previous)));
    auto* frame_address = cleanup.CreateConstGEP1_64(cleanup.getInt8Ty(), execution,
                                                     NativeMemoryLayout::kExecutionFrame);
    cleanup.CreateStore(previous, frame_address);
    cleanup.CreateStore(depth, depth_address);
    cleanup.CreateBr(restored);
    cleanup.SetInsertPoint(restored);
  }
  if (root_range != nullptr) {
    auto exit =
        module_.getOrInsertFunction(runtime::kNativeRootsExitSymbol, cleanup.getVoidTy(), pointer);
    cleanup.CreateCall(exit, {root_range});
  } else if (resources != nullptr) {
    auto release = module_.getOrInsertFunction(runtime::kNativeFrameReleaseSymbol,
                                               cleanup.getVoidTy(), pointer);
    cleanup.CreateCall(release, {resources});
  }
  cleanup.CreateRet(status_);
}

void FunctionEmitter::PrepareTraceFrame() {
  using runtime::NativeMemoryLayout;
  using runtime::NativeTraceFrame;
  auto& context = module_.getContext();
  auto* initialize = llvm::BasicBlock::Create(context, "frame.initialize", body_);
  auto* ready = llvm::BasicBlock::Create(context, "frame.ready", body_);
  builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), trace_initialized_), ready,
                        initialize);
  builder_.SetInsertPoint(initialize);
  // 入口始终检查栈预算；只有可观察边界才链接逻辑帧。纯返回分支不写执行栈，
  // 嵌套调用、异常、调试和恢复仍看到完整调用链，且一次调用最多登记一次。
  auto* execution = body_->getArg(0);
  auto* frame_address = ByteAddress(execution, NativeMemoryLayout::kExecutionFrame);
  auto* pointer = builder_.getPtrTy();
  auto* previous = builder_.CreateLoad(pointer, frame_address);
  auto* frame = trace_frame_;
  builder_.CreateStore(previous, ByteAddress(frame, offsetof(NativeTraceFrame, previous)));
  builder_.CreateStore(image_, ByteAddress(frame, offsetof(NativeTraceFrame, image)));
  auto* null_pointer = llvm::ConstantPointerNull::get(pointer);
  builder_.CreateStore(null_pointer, ByteAddress(frame, offsetof(NativeTraceFrame, checkpoint)));
  builder_.CreateStore(null_pointer, ByteAddress(frame, offsetof(NativeTraceFrame, debug_values)));
  builder_.CreateStore(builder_.getInt64(0),
                       ByteAddress(frame, offsetof(NativeTraceFrame, debug_count)));
  builder_.CreateStore(builder_.getInt32(static_cast<std::uint32_t>(function_index_)),
                       ByteAddress(frame, offsetof(NativeTraceFrame, position) +
                                              offsetof(runtime::TracePosition, function)));
  builder_.CreateStore(frame, frame_address);
  auto* depth_address = ByteAddress(execution, NativeMemoryLayout::kExecutionDepth);
  builder_.CreateStore(builder_.CreateAdd(entry_depth_, builder_.getInt64(1)), depth_address);
  InitializeDebugValues();
  builder_.CreateStore(builder_.getTrue(), trace_initialized_);
  builder_.CreateBr(ready);
  builder_.SetInsertPoint(ready);
}

void FunctionEmitter::CacheImports() {
  bool inline_import = false;
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    const auto target = CallTarget(index);
    if (!target || imports_.contains(target->function)) {
      continue;
    }
    const auto& callee = program_.functions.at(target->function);
    if (callee.native_symbol.empty()) {
      continue;
    }
    // 绑定随实例创建后不可变；闭包临时切换实例会在返回时恢复，作用域内可复用。
    imports_.emplace(target->function,
                     LoadImport(builder_, module_, body_->getArg(0), target->function));
    inline_import = inline_import || CanInlineImportCall(callee);
  }
  if (inline_import) {
    using runtime::NativeTraceFrame;
    Builder allocation(&body_->getEntryBlock(), body_->getEntryBlock().begin());
    auto* frame = allocation.CreateAlloca(
        llvm::ArrayType::get(builder_.getInt8Ty(), sizeof(NativeTraceFrame)), nullptr,
        "import.frame");
    frame->setAlignment(llvm::Align(alignof(NativeTraceFrame)));
    import_frame_ = frame;
    // 封闭调用没有宿主检查点和调试槽；同一调用者内的宿主调用顺序借用此帧。
    // 重入会构造新的调用者帧，不能覆盖仍在执行的外层宿主帧。
    builder_.CreateStore(trace_frame_, ByteAddress(frame, offsetof(NativeTraceFrame, previous)));
    builder_.CreateStore(image_, ByteAddress(frame, offsetof(NativeTraceFrame, image)));
    auto* null_pointer = llvm::ConstantPointerNull::get(builder_.getPtrTy());
    builder_.CreateStore(null_pointer, ByteAddress(frame, offsetof(NativeTraceFrame, checkpoint)));
    builder_.CreateStore(null_pointer,
                         ByteAddress(frame, offsetof(NativeTraceFrame, debug_values)));
    builder_.CreateStore(builder_.getInt64(0),
                         ByteAddress(frame, offsetof(NativeTraceFrame, debug_count)));
  }
}

void FunctionEmitter::CacheFields() {
  if (std::ranges::none_of(source_.instructions, [](const auto& instruction) {
        return (instruction.opcode == Opcode::GetField || instruction.opcode == Opcode::SetField) &&
               instruction.field_declaration.has_value();
      })) {
    return;
  }
  if (tier_ != NativeCodeTier::Optimized) {
    auto caches =
        module_.getOrInsertFunction(runtime::kNativeFieldCachesSymbol, builder_.getPtrTy(),
                                    builder_.getPtrTy(), builder_.getInt32Ty());
    field_caches_ = builder_.CreateCall(
        caches, {body_->getArg(0), builder_.getInt32(static_cast<std::uint32_t>(function_index_))});
    return;
  }
  const auto fields = FindFieldTargets(program_);
  absl::flat_hash_map<ir::ClassId, llvm::Value*> classes;
  auto layouts = module_.getOrInsertFunction(runtime::kNativeDirectFieldLayoutsSymbol,
                                             builder_.getPtrTy(), builder_.getPtrTy());
  auto* table = builder_.CreateCall(layouts, {body_->getArg(0)});
  for (const auto& instruction : source_.instructions) {
    if ((instruction.opcode != Opcode::GetField && instruction.opcode != Opcode::SetField) ||
        !instruction.field_declaration) {
      continue;
    }
    const auto declaration = *instruction.field_declaration;
    if (field_layouts_.contains(declaration)) {
      continue;
    }
    const auto target = fields.at(declaration);
    const auto [owner, inserted] = classes.try_emplace(target.owner, nullptr);
    if (inserted) {
      auto* address = builder_.CreateConstGEP1_64(builder_.getPtrTy(), table, target.owner);
      auto* layout = builder_.CreateLoad(builder_.getPtrTy(), address);
      // 具体函数版本的布局表不可变；对象迁移只改变对象的 layout 和字段块。
      layout->setMetadata(llvm::LLVMContext::MD_invariant_load,
                          llvm::MDNode::get(module_.getContext(), {}));
      owner->second = layout;
    }
    field_layouts_.emplace(declaration,
                           DirectField{
                               .layout = owner->second,
                               .slot = target.slot,
                               .count = program_.layouts.classes.at(target.owner).field_count,
                           });
  }
}

void FunctionEmitter::BeginConstruction() {
  auto enter = module_.getOrInsertFunction(runtime::kNativeInitializationEnterSymbol,
                                           builder_.getVoidTy(), builder_.getPtrTy());
  builder_.CreateCall(enter, {body_->getArg(0)});
  auto* depth = builder_.CreateLoad(builder_.getInt32Ty(), construction_depth_);
  builder_.CreateStore(builder_.CreateAdd(depth, builder_.getInt32(1)), construction_depth_);
}

void FunctionEmitter::EndConstruction() {
  auto exit =
      module_.getOrInsertFunction(runtime::kNativeInitializationExitSymbol, builder_.getVoidTy(),
                                  builder_.getPtrTy(), builder_.getInt32Ty());
  builder_.CreateCall(exit, {body_->getArg(0), builder_.getInt32(1)});
  auto* depth = builder_.CreateLoad(builder_.getInt32Ty(), construction_depth_);
  builder_.CreateStore(builder_.CreateSub(depth, builder_.getInt32(1)), construction_depth_);
}

void FunctionEmitter::AbortConstructions() {
  if (construction_depth_ == nullptr) {
    return;
  }
  auto exit =
      module_.getOrInsertFunction(runtime::kNativeInitializationExitSymbol, builder_.getVoidTy(),
                                  builder_.getPtrTy(), builder_.getInt32Ty());
  builder_.CreateCall(
      exit, {body_->getArg(0), builder_.CreateLoad(builder_.getInt32Ty(), construction_depth_)});
  builder_.CreateStore(builder_.getInt32(0), construction_depth_);
}

}  // namespace vela::compiler::native
