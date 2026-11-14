#include "compiler/native/function_emitter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>

#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/global_analysis.h"
#include "compiler/native/memory_regions.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/native/native_operations.h"
#include "runtime/native/native_program_state.h"
#include "runtime/native/native_symbols.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {

llvm::Value* FunctionEmitter::IsGlobalReady(llvm::Value* address) {
  const auto ready = static_cast<std::uint8_t>(runtime::NativeProgramState::GlobalState::Ready);
  return builder_.CreateICmpEQ(LoadMemory(builder_.getInt8Ty(), address, MemoryRegion::GlobalCache),
                               builder_.getInt8(ready));
}

void FunctionEmitter::StoreGlobalReady(llvm::Value* address) {
  const auto ready = static_cast<std::uint8_t>(runtime::NativeProgramState::GlobalState::Ready);
  StoreMemory(builder_.getInt8(ready), address, MemoryRegion::GlobalCache);
}

llvm::Value* FunctionEmitter::GlobalValueAddress(std::uint32_t slot) {
  return ByteAddress(global_values_, slot * sizeof(runtime::Value));
}

void FunctionEmitter::CacheGlobals() {
  if (std::ranges::none_of(source_.instructions, [](const auto& instruction) {
        return instruction.opcode == Opcode::GetGlobal || instruction.opcode == Opcode::SetGlobal;
      })) {
    return;
  }
  using runtime::NativeMemoryLayout;
  auto* pointer = builder_.getPtrTy();
  auto* data = ProgramData();
  // 当前函数版本的全局存储固定；闭包和宿主重入临时切换实例，返回前恢复。
  // reloadable 缓存的代数逐次检查；封闭程序中只读槽的值在确认初始化后复用。
  if (program_.native_mode == NativeProgramMode::Reloadable) {
    global_caches_ =
        builder_.CreateLoad(pointer, ByteAddress(data, NativeMemoryLayout::kProgramGlobalCaches));
    return;
  }
  global_owner_ = builder_.CreateLoad(
      pointer,
      ByteAddress(data, NativeMemoryLayout::kProgramGlobals + NativeMemoryLayout::kValuePayload));
  global_values_ = builder_.CreateLoad(
      pointer, ByteAddress(global_owner_, NativeMemoryLayout::kObjectNativePayload));
  global_states_ =
      builder_.CreateLoad(pointer, ByteAddress(data, NativeMemoryLayout::kProgramGlobalStates));
  Builder allocation(&body_->getEntryBlock(), body_->getEntryBlock().begin());
  const auto readonly_slots = std::span(global_access_.readonly_slots);
  const auto has_readonly_slots = !readonly_slots.empty();
  std::vector<std::uint8_t> ready_slots(program_.global_initializers.size());
  for (std::size_t position = 0; position < source_.instructions.size(); ++position) {
    const auto& instruction = source_.instructions[position];
    const auto slot = instruction.index;
    if (instruction.opcode == Opcode::GetGlobal &&
        (!IsGlobalInitialized(position) || (has_readonly_slots && readonly_slots[slot] != 0))) {
      ready_slots[slot] = 1;
    }
  }
  // 按源指令顺序生成存储，保持产物的确定性。
  for (const auto& instruction : source_.instructions) {
    const auto index = instruction.index;
    if ((instruction.opcode != Opcode::GetGlobal && instruction.opcode != Opcode::SetGlobal) ||
        global_ready_.contains(index) || ready_slots[index] == 0) {
      continue;
    }
    const auto readonly = has_readonly_slots && readonly_slots[index] != 0;
    auto* ready = allocation.CreateAlloca(builder_.getInt1Ty(), nullptr, "global.is_ready");
    auto* initialized = IsGlobalReady(ByteAddress(global_states_, index));
    if (readonly) {
      auto* value =
          allocation.CreateAlloca(NativeRecordType(module_.getContext()), nullptr, "global.value");
      // 未初始化槽也是合法的 Null 记录；只有 Ready 为真时才消费入口保存的值。
      builder_.CreateStore(
          builder_.CreateLoad(NativeRecordType(module_.getContext()), GlobalValueAddress(index)),
          value);
      global_cached_values_.emplace(index, value);
    }
    builder_.CreateStore(initialized, ready);
    global_ready_.emplace(index, ready);
  }
}

void FunctionEmitter::CacheInitializedGlobals(std::size_t callee) {
  if (global_access_.initialized_on_return.empty()) {
    return;
  }
  for (const auto slot : global_access_.initialized_on_return[callee]) {
    const auto ready = global_ready_.find(slot);
    if (ready == global_ready_.end()) {
      continue;
    }
    if (const auto cached = global_cached_values_.find(slot);
        cached != global_cached_values_.end()) {
      // 被调用方可能刚完成初始化；发布本地 Ready 之前必须刷新入口尚不存在的值。
      auto* value =
          builder_.CreateLoad(NativeRecordType(module_.getContext()), GlobalValueAddress(slot));
      builder_.CreateStore(value, cached->second);
    }
    builder_.CreateStore(builder_.getTrue(), ready->second);
  }
}

void FunctionEmitter::EmitGlobal(const ir::Instruction& instruction) {
  if (program_.native_mode == NativeProgramMode::Reloadable) {
    EmitReloadableGlobal(instruction);
    return;
  }
  if (instruction.opcode == Opcode::GetGlobal) {
    EmitGlobalLoad(instruction);
  } else {
    EmitGlobalStore(instruction);
  }
}

void FunctionEmitter::EmitReloadableGlobal(const ir::Instruction& instruction) {
  using runtime::NativeGlobalCache;
  const bool reading = instruction.opcode == Opcode::GetGlobal;
  auto* cache = ByteAddress(global_caches_, instruction.index * sizeof(NativeGlobalCache));
  auto* epoch_address =
      LoadMemory(builder_.getPtrTy(), ByteAddress(cache, offsetof(NativeGlobalCache, epoch)),
                 MemoryRegion::GlobalCache);
  auto* observed =
      LoadMemory(builder_.getInt64Ty(), ByteAddress(cache, offsetof(NativeGlobalCache, observed)),
                 MemoryRegion::GlobalCache);
  auto* epoch = LoadMemory(builder_.getInt64Ty(), epoch_address, MemoryRegion::GlobalCache);
  auto& context = module_.getContext();
  auto* bound = llvm::BasicBlock::Create(context, "global.bound", body_);
  auto* ready = llvm::BasicBlock::Create(context, "global.ready", body_);
  auto* resolve = llvm::BasicBlock::Create(context, "global.resolve", body_);
  auto* done = llvm::BasicBlock::Create(context, "global.done", body_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(epoch, observed), bound, resolve,
                        llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(bound);
  // 缓存槽只借用当前快照；验证发布代数后才能读取可能已被释放的状态和值地址。
  auto* state_address =
      LoadMemory(builder_.getPtrTy(), ByteAddress(cache, offsetof(NativeGlobalCache, state)),
                 MemoryRegion::GlobalCache);
  if (reading) {
    builder_.CreateCondBr(IsGlobalReady(state_address), ready, resolve,
                          llvm::MDBuilder(context).createLikelyBranchWeights());
  } else {
    builder_.CreateBr(ready);
  }
  builder_.SetInsertPoint(ready);
  auto* value =
      LoadMemory(builder_.getPtrTy(), ByteAddress(cache, offsetof(NativeGlobalCache, value)),
                 MemoryRegion::GlobalCache);
  if (reading) {
    const auto& type = source_.value_types.at(instruction.dst);
    Store(instruction.dst, ReadHeapValue(value, type), type);
  } else {
    auto* owner =
        LoadMemory(builder_.getPtrTy(), ByteAddress(cache, offsetof(NativeGlobalCache, owner)),
                   MemoryRegion::GlobalCache);
    auto stored = LoadStoredValue(instruction.lhs);
    WriteStoredValue(RecordAddress(value, MemoryRegion::Values), stored);
    EmitWriteBarrier(owner, stored);
    StoreGlobalReady(state_address);
  }
  builder_.CreateBr(done);
  builder_.SetInsertPoint(resolve);
  if (reading) {
    PrepareInlineRuntimeCall();
    auto helper = module_.getOrInsertFunction(runtime::kNativeLoadGlobalSymbol,
                                              builder_.getInt8Ty(), builder_.getPtrTy(),
                                              builder_.getInt32Ty(), builder_.getPtrTy());
    CompleteCall(builder_.CreateCall(
                     helper, {body_->getArg(0), builder_.getInt32(instruction.index), result_}),
                 instruction.dst);
    PollAfterRuntimeCall();
  } else {
    // 赋值不执行初始化器，也不增加安全点；缓存失效时只解析当前版本槽并完成写入。
    auto helper = module_.getOrInsertFunction(runtime::kNativeStoreGlobalSymbol,
                                              builder_.getInt8Ty(), builder_.getPtrTy(),
                                              builder_.getInt32Ty(), builder_.getPtrTy());
    CheckStatus(builder_.CreateCall(helper, {
                                                body_->getArg(0),
                                                builder_.getInt32(instruction.index),
                                                Pack(Encode(instruction.lhs), 0),
                                            }));
  }
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
}

void FunctionEmitter::MarkGlobalInitialized(std::uint32_t slot) {
  if (const auto ready = global_ready_.find(slot); ready != global_ready_.end()) {
    builder_.CreateStore(builder_.getTrue(), ready->second);
  }
}

bool FunctionEmitter::IsGlobalInitialized(std::size_t index) const {
  const auto& initialized = global_access_.initialized_accesses;
  return !initialized.empty() && initialized[function_index_][origins_[index]] != 0;
}

void FunctionEmitter::EmitGlobalLoad(const ir::Instruction& instruction) {
  if (IsGlobalInitialized() && !global_cached_values_.contains(instruction.index)) {
    const auto& type = source_.value_types.at(instruction.dst);
    Store(instruction.dst, ReadHeapValue(GlobalValueAddress(instruction.index), type), type);
    MarkGlobalInitialized(instruction.index);
    return;
  }
  auto* pointer = builder_.getPtrTy();
  auto* execution = body_->getArg(0);
  auto* known_ready = global_ready_.at(instruction.index);
  auto* ready = llvm::BasicBlock::Create(module_.getContext(), "global.ready", body_);
  auto* initialize = llvm::BasicBlock::Create(module_.getContext(), "global.initialize", body_);
  auto* loaded = llvm::BasicBlock::Create(module_.getContext(), "global.loaded", body_);
  const auto cached_value = global_cached_values_.find(instruction.index);
  llvm::BasicBlock* cached = nullptr;
  llvm::BasicBlock* complete = nullptr;
  if (cached_value != global_cached_values_.end()) {
    cached = llvm::BasicBlock::Create(module_.getContext(), "global.cached", body_);
    complete = llvm::BasicBlock::Create(module_.getContext(), "global.complete", body_);
  }
  // 本次调用成功读取或写入后，该槽不会重新执行初始化；宿主重入仍可修改槽内值。
  // 局部标记由 LLVM 提升为 SSA，异常初始化不发布 Ready，恢复入口从未知状态重新开始。
  auto* established = builder_.CreateLoad(builder_.getInt1Ty(), known_ready);
  auto* inspect = llvm::BasicBlock::Create(module_.getContext(), "global.inspect", body_);
  auto* branch = builder_.CreateCondBr(established, cached != nullptr ? cached : ready, inspect);
  MarkClosedInvariantCall(*branch);
  builder_.SetInsertPoint(inspect);
  builder_.CreateCondBr(IsGlobalReady(ByteAddress(global_states_, instruction.index)), ready,
                        initialize,
                        llvm::MDBuilder(module_.getContext()).createLikelyBranchWeights());
  builder_.SetInsertPoint(ready);
  // Ready 之后只读 GC 管理的固定槽；分支内不调用、不分配，不需要解码或额外保根。
  const auto& type = source_.value_types.at(instruction.dst);
  Store(instruction.dst, ReadHeapValue(GlobalValueAddress(instruction.index), type), type);
  builder_.CreateBr(loaded);
  builder_.SetInsertPoint(initialize);
  // 惰性初始化、循环初始化与失败重试仍由状态所有者处理。
  PrepareRuntimeCall();
  auto helper = module_.getOrInsertFunction(runtime::kNativeInitializeGlobalSymbol,
                                            builder_.getInt8Ty(), pointer, builder_.getInt32Ty(),
                                            builder_.getInt64Ty(), builder_.getInt8Ty(), pointer);
  const auto frame_kind =
      leaf_entry_ ? runtime::NativeGlobalFrame::Leaf : runtime::NativeGlobalFrame::Current;
  CheckStatus(
      builder_.CreateCall(helper, {
                                      execution,
                                      builder_.getInt32(instruction.index),
                                      builder_.getInt64(TracePosition()),
                                      builder_.getInt8(static_cast<std::uint8_t>(frame_kind)),
                                      result_,
                                  }));
  Unpack(instruction.dst);
  builder_.CreateBr(loaded);
  builder_.SetInsertPoint(loaded);
  builder_.CreateStore(builder_.getTrue(), known_ready);
  if (cached != nullptr) {
    // 闭合 AOT 的无赋值槽只缓存第一次成功读取；初始化失败与其对象内容均不缓存。
    builder_.CreateStore(Encode(instruction.dst), cached_value->second);
    builder_.CreateBr(complete);
    builder_.SetInsertPoint(cached);
    auto* value = builder_.CreateLoad(NativeRecordType(module_.getContext()), cached_value->second);
    auto* decoded = DecodeValue(builder_, value, type);
    AssumeNonNull(builder_, decoded, type);
    Store(instruction.dst, decoded, type);
    builder_.CreateBr(complete);
    builder_.SetInsertPoint(complete);
  }
}

void FunctionEmitter::EmitGlobalStore(const ir::Instruction& instruction) {
  auto value = LoadStoredValue(instruction.lhs);
  WriteStoredValue(RecordAddress(GlobalValueAddress(instruction.index), MemoryRegion::Values),
                   value);
  EmitWriteBarrier(global_owner_, value);
  // 赋值直接建立 Ready 状态，不执行原初始化器；写屏障仍由槽所属对象承担。
  if (!IsGlobalInitialized()) {
    StoreGlobalReady(ByteAddress(global_states_, instruction.index));
  }
  MarkGlobalInitialized(instruction.index);
}

}  // namespace vela::compiler::native
