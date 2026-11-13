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
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/field_access.h"
#include "compiler/native/memory_regions.h"
#include "compiler/native/native_values.h"
#include "runtime/heap/heap.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {

std::size_t FunctionEmitter::PlanAuxiliaryStorage() {
  if (!auxiliary_bytes_) {
    const auto array_bytes = PlanFixedArrayStorage();
    auxiliary_bytes_ = array_bytes + (placement_ == NativeCodePlacement::Linked
                                          ? PlanStringViewStorage() + PlanMapLookupStorage()
                                          : 0);
  }
  return *auxiliary_bytes_;
}

llvm::Value* FunctionEmitter::AllocateAuxiliary(std::size_t bytes) {
  auto* address = ByteAddress(auxiliary_storage_, auxiliary_offset_);
  const auto alignment = runtime::kNativeFrameAlignment;
  auxiliary_offset_ += ((bytes + alignment - 1) / alignment) * alignment;
  return address;
}

llvm::Value* FunctionEmitter::HasStackBudget(llvm::Value* depth, std::uint64_t reserved_bytes) {
  using runtime::NativeMemoryLayout;
  auto* word = builder_.getInt64Ty();
  auto* stack_limit = builder_.CreateLoad(
      word, ByteAddress(body_->getArg(0), NativeMemoryLayout::kExecutionStackLimit));
  MarkInvariant(*stack_limit);
  auto* stack_position = builder_.CreateIntrinsic(llvm::Intrinsic::frameaddress,
                                                  {builder_.getPtrTy()}, {builder_.getInt32(0)});
  auto* stack_available =
      builder_.CreateICmpUGT(builder_.CreatePtrToInt(stack_position, word),
                             builder_.CreateAdd(stack_limit, builder_.getInt64(reserved_bytes)));
  return builder_.CreateAnd(builder_.CreateICmpULT(depth, builder_.getInt64(kMaxCallFrames)),
                            stack_available);
}

bool FunctionEmitter::CanUseLeafEntry() const {
  if (!CanUseSynchronousStackFrame() ||
      (program_.native_mode == NativeProgramMode::Reloadable && kind_ != NativeEntryKind::Leaf) ||
      !IsScalar(source_.return_type)) {
    return false;
  }
  return std::ranges::all_of(source_.instructions, [&](const ir::Instruction& instruction) {
    // 标量 return 不装箱；字段和数组读写可保留副作用，但不能进入用户代码或分配。
    if (instruction.opcode == Opcode::Return) {
      return true;
    }
    if (kind_ == NativeEntryKind::Leaf && generated_[function_index_].budgeted_recursion &&
        instruction.opcode == Opcode::Call && instruction.index == function_index_) {
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

bool FunctionEmitter::CanUseSynchronousStackFrame() const {
  return !roots_.debug_values && ir::CanAnalyzeSynchronousFunction(source_) &&
         frame_bytes_ <= kMaximumNativeStackBytes;
}

bool FunctionEmitter::CanDeferRoots() const {
  if (leaf_entry_ || roots_.count == 0 || kind_ != NativeEntryKind::Initial ||
      !IsClosedLinkedProgram(program_.native_mode, placement_) || !CanUseSynchronousStackFrame()) {
    return false;
  }
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    if (!CanExecuteWithoutCollection(index)) {
      return false;
    }
  }
  return true;
}

bool FunctionEmitter::CanDelayRootRegistration() const {
  if (tier_ != NativeCodeTier::Optimized ||
      (kind_ != NativeEntryKind::Initial && kind_ != NativeEntryKind::BoundedReflection) ||
      !roots_.bounded_polls || !CanUseSynchronousStackFrame()) {
    return false;
  }
  // 无循环帧在首次可观察边界统一挂链。循环仍在入口登记，保持其根链不变量。
  for (std::size_t index = 0; index < source_.instructions.size(); ++index) {
    if (ir::IsBackJump(source_.instructions[index], index)) {
      return false;
    }
  }
  return true;
}

bool FunctionEmitter::CanExecuteWithoutCollection(std::size_t index) const {
  const auto& instruction = source_.instructions[index];
  if (roots_.safepoints[index] != 0 || instruction.construction ||
      instruction.completes_construction) {
    return false;
  }
  switch (instruction.opcode) {
    case Opcode::Call:
      // 保留逻辑帧的托管自递归复用已证明的栈布局；其他入口仍可能分配帧资源。
      return instruction.index == function_index_ &&
             (escape_.borrowed_calls.empty() || !escape_.borrowed_calls[index].has_value());
    case Opcode::GetGlobal:
      return IsGlobalInitialized(index);
    case Opcode::GetField:
    case Opcode::SetField:
      return !instruction.field_declaration.has_value();
    case Opcode::SetGlobal:
    case Opcode::Return:
    case Opcode::Jump:
    case Opcode::JumpIfFalse:
    case Opcode::JumpIfNull:
      return true;
    case Opcode::EqualValue:
    case Opcode::NotEqualValue: {
      const auto& left = source_.value_types[instruction.lhs];
      const auto& right = source_.value_types[instruction.rhs];
      return left.kind == Kind::Null || right.kind == Kind::Null ||
             (IsScalar(left) && IsScalar(right));
    }
    case Opcode::BuiltinCall:
    case Opcode::EqualString:
    case Opcode::NotEqualString:
      return false;
    default: {
      const auto effects = ir::AnalyzeEffects(instruction);
      return !effects.MayCollectOnSuccess() && !effects.reads_state && !effects.writes_state &&
             !effects.changes_control;
    }
  }
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
    auto* position = failure.CreateStore(
        failure.getInt64(runtime::EncodeNativeTracePosition(
            {.function = static_cast<std::uint32_t>(function_index_), .instruction = 0})),
        body_->getArg(static_cast<unsigned>(body_->arg_size() - 1)));
    position->setMetadata(llvm::LLVMContext::MD_tbaa,
                          NativeMemoryTag(context, MemoryRegion::Values));
  }
  failure.CreateRet(error);
  // execution 及固定栈状态由真实入口验证；每次内部调用仍检查动态栈预算。
  llvm::Value* depth = nullptr;
  if (kind_ == NativeEntryKind::Leaf) {
    depth = body_->getArg(LeafEntryLayout(source_.parameter_count).depth);
  } else {
    depth = LoadMemory(word, ByteAddress(execution, NativeMemoryLayout::kExecutionDepth),
                       MemoryRegion::Execution);
  }
  entry_depth_ = depth;
  auto* available = llvm::BasicBlock::Create(context, "depth.available", body_);
  error->addIncoming(Status(builder_, ExecutionStatus::StackOverflow), builder_.GetInsertBlock());
  auto* budget = kind_ == NativeEntryKind::Leaf && generated_[function_index_].budgeted_recursion
                     ? builder_.getTrue()
                     : HasStackBudget(depth);
  builder_.CreateCondBr(budget, available, failed,
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
  llvm::Value* stack_roots_head = nullptr;
  llvm::Value* previous_stack_roots = nullptr;
  const bool on_stack = frame_bytes_ <= kMaximumNativeStackBytes;
  if (IsRecovering()) {
    // 优化帧已预留基线存储，并持有根区间直到恢复返回；此处不分配资源或重复登记根。
  } else if ((leaf_entry_ || deferred_roots_) && roots_.count != 0) {
    // 成功路径没有 GC 安全点；失败分支只写活动前缀，由异常入口临时登记。
    root_slots_ = allocate(roots_.count * sizeof(runtime::Value), alignof(runtime::Value));
  } else if (on_stack && roots_.count != 0) {
    root_slots_ = allocate(roots_.count * sizeof(runtime::Value), alignof(runtime::Value));
    if (!source_.is_async) {
      using runtime::NativeStackRoots;
      auto* frame = allocate(sizeof(NativeStackRoots), alignof(NativeStackRoots));
      auto* head = builder_.CreateLoad(
          pointer, ByteAddress(execution, NativeMemoryLayout::kExecutionStackRoots));
      MarkInvariant(*head);
      stack_roots_head_ = head;
      stack_roots_frame_ = frame;
      delayed_root_registration_ = CanDelayRootRegistration();
      if (!delayed_root_registration_) {
        previous_stack_roots = LinkStackRoots();
        stack_roots_head = head;
      }
    } else {
      root_range = allocate(sizeof(runtime::RootRange), alignof(runtime::RootRange));
      auto enter = module_.getOrInsertFunction(
          runtime::kNativeRootsEnterSymbol, builder_.getVoidTy(), pointer, pointer, word, pointer);
      builder_.CreateCall(enter,
                          {execution, root_slots_, builder_.getInt64(roots_.count), root_range});
    }
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
    ExitInitializations(cleanup, cleanup.CreateLoad(cleanup.getInt32Ty(), construction_depth_));
  }
  if (!leaf_entry_) {
    auto* frame = allocate(sizeof(NativeTraceFrame), alignof(NativeTraceFrame));
    EmitTraceFrame(frame, cleanup, depth);
  }
  if (stack_roots_head != nullptr) {
    auto* restore_roots = cleanup.CreateStore(previous_stack_roots, stack_roots_head);
    restore_roots->setMetadata(llvm::LLVMContext::MD_tbaa, MemoryTag(MemoryRegion::StackRoots));
  } else if (root_range != nullptr) {
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

void FunctionEmitter::EmitTraceFrame(llvm::Value* frame, Builder& cleanup, llvm::Value* depth) {
  using runtime::NativeTraceFrame;
  trace_frame_ = frame;
  if (!delayed_root_registration_) {
    InitializeTraceFrame(frame);
  }
  Builder allocation(&body_->getEntryBlock(), body_->getEntryBlock().begin());
  trace_initialized_ = allocation.CreateAlloca(builder_.getInt1Ty());
  builder_.CreateStore(builder_.getFalse(), trace_initialized_);
  instruction_slot_ = ByteAddress(
      frame, offsetof(NativeTraceFrame, position) + offsetof(runtime::TracePosition, instruction));
  auto& context = module_.getContext();
  auto* restore = llvm::BasicBlock::Create(context, "frame.restore", body_);
  auto* restored = llvm::BasicBlock::Create(context, "frame.restored", body_);
  cleanup.CreateCondBr(cleanup.CreateLoad(cleanup.getInt1Ty(), trace_initialized_), restore,
                       restored);
  cleanup.SetInsertPoint(restore);
  auto* previous = cleanup.CreateLoad(
      cleanup.getPtrTy(),
      cleanup.CreateConstGEP1_64(cleanup.getInt8Ty(), frame, offsetof(NativeTraceFrame, previous)));
  StoreExecutionState(cleanup, previous, depth);
  if (delayed_root_registration_) {
    auto* previous_roots = cleanup.CreateLoad(
        cleanup.getPtrTy(),
        cleanup.CreateConstGEP1_64(cleanup.getInt8Ty(), stack_roots_frame_,
                                   offsetof(runtime::NativeStackRoots, previous)));
    cleanup.CreateStore(previous_roots, stack_roots_head_)
        ->setMetadata(llvm::LLVMContext::MD_tbaa, MemoryTag(MemoryRegion::StackRoots));
  }
  cleanup.CreateBr(restored);
  cleanup.SetInsertPoint(restored);
}

llvm::Value* FunctionEmitter::LinkStackRoots() {
  using runtime::NativeStackRoots;
  auto* frame = stack_roots_frame_;
  auto* previous = LoadMemory(builder_.getPtrTy(), stack_roots_head_, MemoryRegion::StackRoots);
  // 同步帧和重载恢复按栈序退出，共用 Heap 的 LIFO 根链。
  // 首次挂链前清空全部槽，随后每个实际 GC 边界只保存其活动值。
  static_assert(static_cast<std::uint8_t>(runtime::ValueKind::Null) == 0);
  builder_.CreateMemSet(root_slots_, builder_.getInt8(0), roots_.count * sizeof(runtime::Value),
                        llvm::Align(alignof(runtime::Value)));
  builder_.CreateStore(root_slots_, ByteAddress(frame, offsetof(NativeStackRoots, values)));
  builder_.CreateStore(builder_.getInt64(roots_.count),
                       ByteAddress(frame, offsetof(NativeStackRoots, count)));
  builder_.CreateStore(previous, ByteAddress(frame, offsetof(NativeStackRoots, previous)));
  StoreMemory(frame, stack_roots_head_, MemoryRegion::StackRoots);
  return previous;
}

void FunctionEmitter::InitializeTraceFrame(llvm::Value* frame) {
  using runtime::NativeTraceFrame;
  builder_.CreateStore(image_, ByteAddress(frame, offsetof(NativeTraceFrame, image)));
  auto* null_pointer = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  builder_.CreateStore(null_pointer, ByteAddress(frame, offsetof(NativeTraceFrame, checkpoint)));
  builder_.CreateStore(null_pointer, ByteAddress(frame, offsetof(NativeTraceFrame, debug_values)));
  builder_.CreateStore(builder_.getInt64(0),
                       ByteAddress(frame, offsetof(NativeTraceFrame, debug_count)));
  builder_.CreateStore(builder_.getInt32(static_cast<std::uint32_t>(function_index_)),
                       ByteAddress(frame, offsetof(NativeTraceFrame, position) +
                                              offsetof(runtime::TracePosition, function)));
  InitializeDebugValues();
}

void FunctionEmitter::StoreExecutionState(Builder& builder,
                                          llvm::Value* frame,
                                          llvm::Value* depth) {
  using runtime::NativeMemoryLayout;
  auto* execution = body_->getArg(0);
  auto* tag = MemoryTag(MemoryRegion::Execution);
  auto* frame_store =
      builder.CreateStore(frame, builder.CreateConstGEP1_64(builder.getInt8Ty(), execution,
                                                            NativeMemoryLayout::kExecutionFrame));
  frame_store->setMetadata(llvm::LLVMContext::MD_tbaa, tag);
  auto* depth_store =
      builder.CreateStore(depth, builder.CreateConstGEP1_64(builder.getInt8Ty(), execution,
                                                            NativeMemoryLayout::kExecutionDepth));
  depth_store->setMetadata(llvm::LLVMContext::MD_tbaa, tag);
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
  auto* previous = LoadMemory(pointer, frame_address, MemoryRegion::Execution);
  auto* frame = trace_frame_;
  if (delayed_root_registration_) {
    InitializeTraceFrame(frame);
    LinkStackRoots();
  }
  StoreMemory(previous, ByteAddress(frame, offsetof(NativeTraceFrame, previous)),
              MemoryRegion::Execution);
  auto* next_depth = builder_.CreateAdd(entry_depth_, builder_.getInt64(1));
  StoreExecutionState(builder_, frame, next_depth);
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
                     LoadImport(builder_, module_, body_->getArg(0), target->function, placement_));
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
    InitializeTraceFrame(frame);
    builder_.CreateStore(trace_frame_, ByteAddress(frame, offsetof(NativeTraceFrame, previous)));
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
  absl::flat_hash_map<ir::ClassId, llvm::Value*> classes;
  auto* program = LoadProgramData(builder_, body_->getArg(0));
  auto* table = builder_.CreateLoad(
      builder_.getPtrTy(),
      ByteAddress(program, runtime::NativeMemoryLayout::kProgramDirectFieldLayouts));
  MarkInvariant(*table);
  for (const auto& instruction : source_.instructions) {
    if ((instruction.opcode != Opcode::GetField && instruction.opcode != Opcode::SetField) ||
        !instruction.field_declaration) {
      continue;
    }
    const auto declaration = *instruction.field_declaration;
    if (field_layouts_.contains(declaration)) {
      continue;
    }
    const auto target = field_targets_.at(declaration);
    const auto [owner, inserted] = classes.try_emplace(target.owner, nullptr);
    if (inserted) {
      auto* address = builder_.CreateConstGEP1_64(builder_.getPtrTy(), table, target.owner);
      auto* layout = builder_.CreateLoad(builder_.getPtrTy(), address);
      // 具体函数版本的布局表不可变；对象迁移只改变对象的 layout 和字段块。
      MarkInvariant(*layout);
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

llvm::Value* FunctionEmitter::InitializationsAddress(Builder& builder) {
  return builder.CreateConstGEP1_64(builder.getInt8Ty(), ControlState(),
                                    offsetof(runtime::NativeControlState, initializations));
}

void FunctionEmitter::BeginConstruction() {
  auto* address = InitializationsAddress(builder_);
  auto* active = builder_.CreateLoad(builder_.getInt64Ty(), address);
  builder_.CreateStore(builder_.CreateAdd(active, builder_.getInt64(1)), address);
  auto* depth = builder_.CreateLoad(builder_.getInt32Ty(), construction_depth_);
  builder_.CreateStore(builder_.CreateAdd(depth, builder_.getInt32(1)), construction_depth_);
}

void FunctionEmitter::ExitInitializations(Builder& builder, llvm::Value* count) {
  auto* address = InitializationsAddress(builder);
  auto* active = builder.CreateLoad(builder.getInt64Ty(), address);
  builder.CreateStore(builder.CreateSub(active, builder.CreateZExt(count, builder.getInt64Ty())),
                      address);
}

void FunctionEmitter::EndConstruction() {
  ExitInitializations(builder_, builder_.getInt32(1));
  auto* depth = builder_.CreateLoad(builder_.getInt32Ty(), construction_depth_);
  builder_.CreateStore(builder_.CreateSub(depth, builder_.getInt32(1)), construction_depth_);
}

void FunctionEmitter::AbortConstructions() {
  if (construction_depth_ == nullptr) {
    return;
  }
  ExitInitializations(builder_, builder_.CreateLoad(builder_.getInt32Ty(), construction_depth_));
  builder_.CreateStore(builder_.getInt32(0), construction_depth_);
}

}  // namespace vela::compiler::native
