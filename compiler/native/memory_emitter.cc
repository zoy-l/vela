#include "base/native_program_mode.h"
#include "compiler/native/function_emitter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_values.h"
#include "runtime/heap/field_storage.h"
#include "runtime/heap/instance_storage.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/native/native_operations.h"
#include "runtime/native/native_program_state.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {
namespace {

SourceType ElementValueType(const layout::Type& element) {
  SourceType type{.kind = Kind::Class, .name = {}, .nullable = element.nullable};
  using ElementKind = layout::Type::Kind;
  switch (element.kind) {
    case ElementKind::Dynamic:
      type.kind = Kind::Dynamic;
      break;
    case ElementKind::Boolean:
      type.kind = Kind::Bool;
      break;
    case ElementKind::Integer:
      type.kind = Kind::Int;
      break;
    case ElementKind::Enum:
      type.kind = Kind::Enum;
      break;
    case ElementKind::Double:
      type.kind = Kind::Double;
      break;
    case ElementKind::String:
      type.kind = Kind::String;
      break;
    default:
      break;
  }
  return type;
}

}  // namespace

llvm::Value* FunctionEmitter::ByteAddress(llvm::Value* object, std::uint64_t offset) {
  return builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, offset);
}

llvm::MDNode* FunctionEmitter::MemoryTag(MemoryRegion region) {
  constexpr std::array kNames{
      "heap header",  "class layout",   "field cache",    "instance header",
      "field states", "managed values", "array elements", "global cache",
      "shadow roots", "GC state",       "field payloads", "field kinds",
  };
  llvm::MDBuilder metadata(module_.getContext());
  auto* root = metadata.createTBAARoot("Vela native storage");
  auto* type = metadata.createTBAAScalarTypeNode(kNames.at(static_cast<std::size_t>(region)), root);
  return metadata.createTBAAStructTagNode(type, type, 0);
}

llvm::LoadInst* FunctionEmitter::LoadMemory(llvm::Type* type,
                                            llvm::Value* address,
                                            MemoryRegion region) {
  auto* load = builder_.CreateLoad(type, address);
  load->setMetadata(llvm::LLVMContext::MD_tbaa, MemoryTag(region));
  return load;
}

llvm::StoreInst* FunctionEmitter::StoreMemory(llvm::Value* value,
                                              llvm::Value* address,
                                              MemoryRegion region) {
  auto* store = builder_.CreateStore(value, address);
  store->setMetadata(llvm::LLVMContext::MD_tbaa, MemoryTag(region));
  return store;
}

FunctionEmitter::StoredValueAddress FunctionEmitter::RecordAddress(llvm::Value* address,
                                                                   MemoryRegion region) {
  using runtime::NativeMemoryLayout;
  return {.payload = ByteAddress(address, NativeMemoryLayout::kValuePayload),
          .kind = ByteAddress(address, NativeMemoryLayout::kValueKind),
          .payload_region = region,
          .kind_region = region};
}

FunctionEmitter::StoredValueAddress FunctionEmitter::FieldAddress(llvm::Value* storage,
                                                                  llvm::Value* index,
                                                                  llvm::Value* kind_offset) {
  using runtime::FieldStorage;
  auto* wide_index = builder_.CreateZExtOrTrunc(index, builder_.getInt64Ty());
  auto* payload_offset =
      builder_.CreateMul(wide_index, builder_.getInt64(FieldStorage::kPayloadBytes));
  return {.payload = builder_.CreateGEP(builder_.getInt8Ty(), storage, payload_offset),
          .kind = builder_.CreateGEP(builder_.getInt8Ty(), storage, kind_offset),
          .payload_region = MemoryRegion::FieldPayloads,
          .kind_region = MemoryRegion::FieldKinds};
}

llvm::Value* FunctionEmitter::ReadHeapValue(llvm::Value* address, const SourceType& type) {
  return ReadStoredValue(RecordAddress(address, MemoryRegion::Values), type);
}

llvm::Value* FunctionEmitter::ReadStoredValue(StoredValueAddress address, const SourceType& type) {
  if (type.kind == Kind::Dynamic) {
    llvm::Value* record = llvm::Constant::getNullValue(NativeRecordType(module_.getContext()));
    record = builder_.CreateInsertValue(
        record, LoadMemory(builder_.getInt64Ty(), address.payload, address.payload_region), 0);
    auto* kind_value = LoadMemory(builder_.getInt8Ty(), address.kind, address.kind_region);
    return builder_.CreateInsertValue(record, kind_value, 1);
  }
  if (type.kind == Kind::Null || type.kind == Kind::Void) {
    return builder_.getFalse();
  }
  auto* storage_type =
      type.kind == Kind::Bool ? builder_.getInt8Ty() : PayloadType(module_.getContext(), type.kind);
  auto* loaded = LoadMemory(storage_type, address.payload, address.payload_region);
  if (IsReferenceKind(type.kind) && !type.nullable) {
    loaded->setMetadata(llvm::LLVMContext::MD_nonnull, llvm::MDNode::get(module_.getContext(), {}));
  }
  llvm::Value* value = loaded;
  if (type.kind == Kind::Bool) {
    value = builder_.CreateICmpNE(value, builder_.getInt8(0));
  }
  if (!HasValidity(type)) {
    return value;
  }
  auto* tag = LoadMemory(builder_.getInt8Ty(), address.kind, address.kind_region);
  auto* present = builder_.CreateICmpNE(tag, builder_.getInt8(NativeKind(Kind::Null)));
  auto* result = builder_.CreateInsertValue(
      llvm::PoisonValue::get(ValueType(module_.getContext(), type)), present, 0);
  return builder_.CreateInsertValue(result, value, 1);
}

void FunctionEmitter::WriteHeapValue(llvm::Value* address,
                                     ir::RegisterId reg,
                                     MemoryRegion region) {
  WriteStoredValue(RecordAddress(address, region), reg);
}

void FunctionEmitter::WriteStoredValue(StoredValueAddress address, ir::RegisterId reg) {
  const auto& type = source_.value_types.at(reg);
  auto* value = Load(reg);
  auto* encoded = EncodeValue(builder_, value, type);
  if (IsReferenceKind(type.kind)) {
    // 用指针类型保留别名信息；不能将托管指针经整数运算后再写入对象。
    StoreMemory(value, address.payload, address.payload_region);
  } else {
    StoreMemory(builder_.CreateExtractValue(encoded, 0), address.payload, address.payload_region);
  }
  StoreMemory(builder_.CreateExtractValue(encoded, 1), address.kind, address.kind_region);
}

void FunctionEmitter::EmitWriteBarrier(llvm::Value* object, ir::RegisterId reg) {
  using runtime::NativeMemoryLayout;
  const auto& type = source_.value_types.at(reg);
  const auto kind = type.kind;
  if (kind != Kind::Dynamic && !IsReferenceKind(kind)) {
    return;
  }
  auto* value = Load(reg);
  auto* encoded = EncodeValue(builder_, value, type);
  llvm::Value* reference = nullptr;
  if (kind == Kind::Dynamic) {
    auto* tag = builder_.CreateExtractValue(encoded, 1);
    auto* reference_kind =
        builder_.CreateOr(builder_.CreateICmpEQ(tag, builder_.getInt8(NativeKind(Kind::Class))),
                          builder_.CreateICmpEQ(tag, builder_.getInt8(NativeKind(Kind::String))));
    value = DecodeValue(builder_, encoded, {.kind = Kind::Class, .name = {}});
    reference = builder_.CreateAnd(reference_kind, builder_.CreateIsNotNull(value));
  } else {
    reference = builder_.CreateIsNotNull(value);
  }
  // 屏障状态和对象标记位只在 Isolate 线程改变。只有已标记 source 指向未标记
  // target 才需进入运行时；Sweep 慢路径仍立即追踪新引用的完整对象图。
  auto* state = builder_.CreateLoad(
      builder_.getPtrTy(),
      ByteAddress(body_->getArg(0), NativeMemoryLayout::kExecutionBarrierState));
  state->setMetadata(llvm::LLVMContext::MD_invariant_load,
                     llvm::MDNode::get(module_.getContext(), {}));
  auto* phase =
      LoadMemory(builder_.getInt8Ty(), ByteAddress(state, offsetof(runtime::GcBarrierState, phase)),
                 MemoryRegion::GcState);
  auto* mark = LoadMemory(builder_.getInt8Ty(),
                          ByteAddress(state, offsetof(runtime::GcBarrierState, mark_bit)),
                          MemoryRegion::GcState);
  auto* source_mark =
      LoadMemory(builder_.getInt8Ty(), ByteAddress(object, NativeMemoryLayout::kObjectMarkBit),
                 MemoryRegion::HeapHeader);
  auto* active = builder_.CreateICmpNE(
      phase, builder_.getInt8(static_cast<std::uint8_t>(runtime::GcPhase::Idle)));
  auto* required = builder_.CreateAnd(
      reference, builder_.CreateAnd(active, builder_.CreateICmpEQ(source_mark, mark)));
  auto* inspect = llvm::BasicBlock::Create(module_.getContext(), "write.inspect", body_);
  auto* barrier = llvm::BasicBlock::Create(module_.getContext(), "write.barrier", body_);
  auto* written = llvm::BasicBlock::Create(module_.getContext(), "write.done", body_);
  builder_.CreateCondBr(required, inspect, written,
                        llvm::MDBuilder(module_.getContext()).createUnlikelyBranchWeights());
  builder_.SetInsertPoint(inspect);
  // 非空引用守卫支配 header 读取；dynamic 可以携带 Object(nullptr) 或 String(nullptr)。
  // Mark 阶段的灰对象会继续扫描；Sweep 在返回脚本前已排空灰队列。
  auto* target_mark =
      LoadMemory(builder_.getInt8Ty(), ByteAddress(value, NativeMemoryLayout::kObjectMarkBit),
                 MemoryRegion::HeapHeader);
  builder_.CreateCondBr(builder_.CreateICmpNE(target_mark, mark), barrier, written,
                        llvm::MDBuilder(module_.getContext()).createUnlikelyBranchWeights());
  builder_.SetInsertPoint(barrier);
  auto helper =
      module_.getOrInsertFunction("vela_native_v1_write_barrier", builder_.getVoidTy(),
                                  builder_.getPtrTy(), builder_.getPtrTy(), builder_.getPtrTy());
  builder_.CreateCall(helper, {body_->getArg(0), object, Pack(encoded, 0)});
  builder_.CreateBr(written);
  builder_.SetInsertPoint(written);
}

void FunctionEmitter::EmitField(const ir::Instruction& instruction) {
  const auto reading = instruction.opcode == Opcode::GetField;
  auto* object = Reference(reading ? instruction.lhs : instruction.dst);
  Check(builder_.CreateIsNotNull(object), ExecutionStatus::TypeError);
  if (instruction.field_declaration) {
    EmitReloadableField(instruction, object);
    return;
  }
  // 字段编号由静态成员解析确定，实际对象布局在入口边界验证；无需逐次查找布局。
  using runtime::NativeMemoryLayout;
  auto* count =
      LoadMemory(builder_.getInt32Ty(), ByteAddress(object, NativeMemoryLayout::kObjectFieldCount),
                 MemoryRegion::HeapHeader);
  count->setMetadata(llvm::LLVMContext::MD_invariant_load,
                     llvm::MDNode::get(module_.getContext(), {}));
  auto* kind_offset = builder_.CreateAdd(
      builder_.CreateMul(builder_.CreateZExt(count, builder_.getInt64Ty()),
                         builder_.getInt64(runtime::FieldStorage::kPayloadBytes)),
      builder_.getInt64(runtime::FieldStorage::KindOffset(0, instruction.index)));
  const auto address = FieldAddress(ByteAddress(object, sizeof(runtime::HeapObject)),
                                    builder_.getInt32(instruction.index), kind_offset);
  if (reading) {
    const auto& type = source_.value_types.at(instruction.dst);
    Store(instruction.dst, ReadStoredValue(address, type), type);
  } else {
    WriteStoredValue(address, instruction.lhs);
    EmitWriteBarrier(object, instruction.lhs);
    if (program_.native_mode == NativeProgramMode::Reloadable && instruction.index == 0 &&
        source_.value_types.at(instruction.dst).kind == Kind::Function) {
      auto helper =
          module_.getOrInsertFunction("vela_native_v3_capture_receiver", builder_.getInt8Ty(),
                                      builder_.getPtrTy(), builder_.getPtrTy());
      CheckStatus(builder_.CreateCall(helper, {body_->getArg(0), object}));
    }
  }
}

void FunctionEmitter::EmitReloadableField(const ir::Instruction& instruction, llvm::Value* object) {
  using runtime::FieldState;
  using runtime::InstanceStorage;
  using runtime::NativeFieldCache;
  using runtime::NativeMemoryLayout;
  const auto reading = instruction.opcode == Opcode::GetField;
  const auto field = *instruction.field_declaration;
  const auto origin = origins_.subspan(instruction_index_).front();
  const auto& canonical = program_.functions.at(function_index_).instructions.at(origin);
  if (!reading &&
      (canonical.opcode != instruction.opcode || canonical.field_declaration != field)) {
    auto helper = module_.getOrInsertFunction(
        "vela_native_v7_store_materialized_field", builder_.getInt8Ty(), builder_.getPtrTy(),
        builder_.getPtrTy(), builder_.getInt64Ty(), builder_.getPtrTy());
    CheckStatus(builder_.CreateCall(helper, {
                                                body_->getArg(0),
                                                object,
                                                builder_.getInt64(field.value),
                                                Pack(Encode(instruction.lhs), 0),
                                            }));
    return;
  }
  if (tier_ == NativeCodeTier::Optimized) {
    EmitOptimizedField(instruction, object);
    return;
  }
  const auto access_index = *field_accesses_.at(origin);
  auto* cache = ByteAddress(field_caches_, access_index * sizeof(NativeFieldCache));
  auto* layout =
      LoadMemory(builder_.getPtrTy(), ByteAddress(object, NativeMemoryLayout::kObjectLayout),
                 MemoryRegion::HeapHeader);
  auto* layout_id =
      LoadMemory(builder_.getInt64Ty(), ByteAddress(layout, NativeMemoryLayout::kClassId),
                 MemoryRegion::ClassLayout);
  layout_id->setMetadata(llvm::LLVMContext::MD_invariant_load,
                         llvm::MDNode::get(module_.getContext(), {}));
  auto* cached_id =
      LoadMemory(builder_.getInt64Ty(), ByteAddress(cache, offsetof(NativeFieldCache, layout_id)),
                 MemoryRegion::FieldCache);
  auto* matches = builder_.CreateICmpEQ(layout_id, cached_id);
  auto& context = module_.getContext();
  auto* cached = llvm::BasicBlock::Create(context, "field.cached", body_);
  auto* ready = llvm::BasicBlock::Create(context, "field.ready", body_);
  auto* resolve = llvm::BasicBlock::Create(context, "field.resolve", body_);
  auto* done = llvm::BasicBlock::Create(context, "field.done", body_);
  builder_.CreateCondBr(matches, cached, resolve,
                        llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(cached);
  auto* slot =
      LoadMemory(builder_.getInt32Ty(), ByteAddress(cache, offsetof(NativeFieldCache, slot)),
                 MemoryRegion::FieldCache);
  auto* kind_offset =
      LoadMemory(builder_.getInt64Ty(), ByteAddress(cache, offsetof(NativeFieldCache, kind_offset)),
                 MemoryRegion::FieldCache);
  // 字段块可在提交时迁移；只在本安全点之后读取，地址不跨任何运行时调用保留。
  auto* storage =
      LoadMemory(builder_.getPtrTy(), ByteAddress(object, NativeMemoryLayout::kObjectNativePayload),
                 MemoryRegion::HeapHeader);
  auto* values = ByteAddress(storage, sizeof(InstanceStorage));
  auto* unready_address = ByteAddress(storage, offsetof(InstanceStorage, unready));
  auto* unready = LoadMemory(builder_.getInt32Ty(), unready_address, MemoryRegion::InstanceHeader);
  auto* inspect = llvm::BasicBlock::Create(context, "field.inspect", body_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(unready, builder_.getInt32(0)), ready, inspect,
                        llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(inspect);
  auto* count =
      LoadMemory(builder_.getInt32Ty(), ByteAddress(storage, offsetof(InstanceStorage, count)),
                 MemoryRegion::InstanceHeader);
  auto* state_offset = builder_.CreateMul(
      builder_.CreateZExt(count, builder_.getInt64Ty()),
      builder_.getInt64(runtime::FieldStorage::kPayloadBytes + runtime::FieldStorage::kKindBytes));
  auto* states = builder_.CreateGEP(builder_.getInt8Ty(), values, state_offset);
  auto* wide_slot = builder_.CreateZExt(slot, builder_.getInt64Ty());
  auto* state_address = builder_.CreateGEP(builder_.getInt8Ty(), states, wide_slot);
  auto* state = LoadMemory(builder_.getInt8Ty(), state_address, MemoryRegion::FieldStates);
  auto* initialized =
      builder_.CreateICmpEQ(state, builder_.getInt8(static_cast<std::uint8_t>(FieldState::Ready)));
  auto* initialize =
      reading ? resolve : llvm::BasicBlock::Create(context, "field.initialize", body_);
  builder_.CreateCondBr(initialized, ready, initialize,
                        llvm::MDBuilder(context).createLikelyBranchWeights());
  if (!reading) {
    builder_.SetInsertPoint(initialize);
    auto* available = llvm::BasicBlock::Create(context, "field.available", body_);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(
            state, builder_.getInt8(static_cast<std::uint8_t>(FieldState::Uninitialized))),
        available, resolve, llvm::MDBuilder(context).createLikelyBranchWeights());
    builder_.SetInsertPoint(available);
    StoreMemory(builder_.CreateNUWSub(unready, builder_.getInt32(1)), unready_address,
                MemoryRegion::InstanceHeader);
    StoreMemory(builder_.getInt8(static_cast<std::uint8_t>(FieldState::Ready)), state_address,
                MemoryRegion::FieldStates);
    builder_.CreateBr(ready);
  }
  builder_.SetInsertPoint(ready);
  // 全部字段就绪后不再读取或写入逐字段状态，直到发布替换整个字段块。
  const auto address = FieldAddress(values, slot, kind_offset);
  if (reading) {
    const auto& type = source_.value_types.at(instruction.dst);
    Store(instruction.dst, ReadStoredValue(address, type), type);
  } else {
    WriteStoredValue(address, instruction.lhs);
    EmitWriteBarrier(object, instruction.lhs);
  }
  builder_.CreateBr(done);
  builder_.SetInsertPoint(resolve);
  if (reading) {
    Poll();
    PrepareRuntimeCall();
  }
  auto helper = module_.getOrInsertFunction(
      reading ? "vela_native_v3_load_field" : "vela_native_v3_store_field", builder_.getInt8Ty(),
      builder_.getPtrTy(), builder_.getPtrTy(), builder_.getInt64Ty(), builder_.getInt64Ty(),
      builder_.getPtrTy());
  const auto access = runtime::EncodeNativeTracePosition(
      {.function = static_cast<std::uint32_t>(function_index_), .instruction = access_index});
  auto* value = reading ? result_ : Pack(Encode(instruction.lhs), 0);
  CheckStatus(builder_.CreateCall(helper, {
                                              body_->getArg(0),
                                              object,
                                              builder_.getInt64(field.value),
                                              builder_.getInt64(access),
                                              value,
                                          }));
  if (reading) {
    Unpack(instruction.dst);
    PollAfterRuntimeCall();
  }
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
}

void FunctionEmitter::EmitOptimizedField(const ir::Instruction& instruction, llvm::Value* object) {
  using runtime::FieldState;
  using runtime::InstanceStorage;
  using runtime::NativeMemoryLayout;
  const auto& direct = field_layouts_.at(*instruction.field_declaration);
  auto& context = module_.getContext();
  auto* layout =
      LoadMemory(builder_.getPtrTy(), ByteAddress(object, NativeMemoryLayout::kObjectLayout),
                 MemoryRegion::HeapHeader);
  auto* matched = llvm::BasicBlock::Create(context, "field.matched", body_);
  auto* ready = llvm::BasicBlock::Create(context, "field.ready", body_);
  auto* recover = llvm::BasicBlock::Create(context, "field.recover", body_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(layout, direct.layout), matched, recover,
                        llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(matched);
  auto* storage =
      LoadMemory(builder_.getPtrTy(), ByteAddress(object, NativeMemoryLayout::kObjectNativePayload),
                 MemoryRegion::HeapHeader);
  auto* unready_address = ByteAddress(storage, offsetof(InstanceStorage, unready));
  auto* unready = LoadMemory(builder_.getInt32Ty(), unready_address, MemoryRegion::InstanceHeader);
  auto* inspect = llvm::BasicBlock::Create(context, "field.inspect", body_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(unready, builder_.getInt32(0)), ready, inspect,
                        llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(inspect);
  auto* state_address =
      ByteAddress(storage, sizeof(InstanceStorage) + runtime::FieldStorage::Size(direct.count) +
                               (direct.slot * sizeof(FieldState)));
  auto* state = LoadMemory(builder_.getInt8Ty(), state_address, MemoryRegion::FieldStates);
  const auto reading = instruction.opcode == Opcode::GetField;
  auto* initialize =
      reading ? recover : llvm::BasicBlock::Create(context, "field.initialize", body_);
  builder_.CreateCondBr(
      builder_.CreateICmpEQ(state, builder_.getInt8(static_cast<std::uint8_t>(FieldState::Ready))),
      ready, initialize, llvm::MDBuilder(context).createLikelyBranchWeights());
  if (!reading) {
    builder_.SetInsertPoint(initialize);
    auto* available = llvm::BasicBlock::Create(context, "field.available", body_);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(
            state, builder_.getInt8(static_cast<std::uint8_t>(FieldState::Uninitialized))),
        available, recover, llvm::MDBuilder(context).createLikelyBranchWeights());
    builder_.SetInsertPoint(available);
    StoreMemory(builder_.CreateNUWSub(unready, builder_.getInt32(1)), unready_address,
                MemoryRegion::InstanceHeader);
    StoreMemory(builder_.getInt8(static_cast<std::uint8_t>(FieldState::Ready)), state_address,
                MemoryRegion::FieldStates);
    builder_.CreateBr(ready);
  }
  builder_.SetInsertPoint(recover);
  // 不执行当前读写；保存规范输入后由基线处理布局变化、惰性初始化和精确异常。
  // 冷路径直接退出当前机器帧，成功路径间不再汇合可能修改堆存储的运行时调用。
  SetPosition();
  StoreRoots(instruction_index_);
  StoreCheckpoint();
  Recover();
  builder_.SetInsertPoint(ready);
  const auto address =
      FieldAddress(ByteAddress(storage, sizeof(InstanceStorage)), builder_.getInt32(direct.slot),
                   builder_.getInt64(runtime::FieldStorage::KindOffset(direct.count, direct.slot)));
  if (reading) {
    const auto& type = source_.value_types.at(instruction.dst);
    Store(instruction.dst, ReadStoredValue(address, type), type);
  } else {
    WriteStoredValue(address, instruction.lhs);
    EmitWriteBarrier(object, instruction.lhs);
  }
}

llvm::Value* FunctionEmitter::ProgramData() {
  using runtime::NativeMemoryLayout;
  auto* pointer = builder_.getPtrTy();
  auto* instance = builder_.CreateLoad(
      pointer, ByteAddress(body_->getArg(0), NativeMemoryLayout::kExecutionInstance));
  return builder_.CreateLoad(pointer, ByteAddress(instance, NativeMemoryLayout::kInstanceData));
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
  using runtime::NativeMemoryLayout;
  const bool reading = instruction.opcode == Opcode::GetGlobal;
  auto* caches = builder_.CreateLoad(
      builder_.getPtrTy(), ByteAddress(ProgramData(), NativeMemoryLayout::kProgramGlobalCaches));
  auto* cache = ByteAddress(caches, instruction.index * sizeof(NativeGlobalCache));
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
    auto* state = LoadMemory(builder_.getInt8Ty(), state_address, MemoryRegion::GlobalCache);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(state, builder_.getInt8(static_cast<std::uint8_t>(
                                         runtime::NativeProgramState::GlobalState::Ready))),
        ready, resolve, llvm::MDBuilder(context).createLikelyBranchWeights());
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
    WriteHeapValue(value, instruction.lhs);
    EmitWriteBarrier(owner, instruction.lhs);
    StoreMemory(builder_.getInt8(
                    static_cast<std::uint8_t>(runtime::NativeProgramState::GlobalState::Ready)),
                state_address, MemoryRegion::GlobalCache);
  }
  builder_.CreateBr(done);
  builder_.SetInsertPoint(resolve);
  if (reading) {
    if (!NeedsBoundaryPoll(instruction_index_)) {
      Poll();
    }
    PrepareRuntimeCall();
    auto helper = module_.getOrInsertFunction("vela_native_v10_load_global", builder_.getInt8Ty(),
                                              builder_.getPtrTy(), builder_.getInt32Ty(),
                                              builder_.getPtrTy());
    CompleteCall(builder_.CreateCall(
                     helper, {body_->getArg(0), builder_.getInt32(instruction.index), result_}),
                 instruction.dst);
    PollAfterRuntimeCall();
  } else {
    // 赋值不执行初始化器，也不增加安全点；缓存失效时只解析当前版本槽并完成写入。
    auto helper = module_.getOrInsertFunction("vela_native_v10_store_global", builder_.getInt8Ty(),
                                              builder_.getPtrTy(), builder_.getInt32Ty(),
                                              builder_.getPtrTy());
    CheckStatus(builder_.CreateCall(helper, {body_->getArg(0), builder_.getInt32(instruction.index),
                                             Pack(Encode(instruction.lhs), 0)}));
  }
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
}

void FunctionEmitter::EmitGlobalLoad(const ir::Instruction& instruction) {
  using runtime::NativeMemoryLayout;
  auto* pointer = builder_.getPtrTy();
  auto* execution = body_->getArg(0);
  auto* data = ProgramData();
  auto* states =
      builder_.CreateLoad(pointer, ByteAddress(data, NativeMemoryLayout::kProgramGlobalStates));
  auto* state = builder_.CreateLoad(builder_.getInt8Ty(), ByteAddress(states, instruction.index));
  auto* ready = llvm::BasicBlock::Create(module_.getContext(), "global.ready", body_);
  auto* initialize = llvm::BasicBlock::Create(module_.getContext(), "global.initialize", body_);
  auto* loaded = llvm::BasicBlock::Create(module_.getContext(), "global.loaded", body_);
  builder_.CreateCondBr(
      builder_.CreateICmpEQ(state, builder_.getInt8(static_cast<std::uint8_t>(
                                       runtime::NativeProgramState::GlobalState::Ready))),
      ready, initialize, llvm::MDBuilder(module_.getContext()).createLikelyBranchWeights());
  builder_.SetInsertPoint(ready);
  // Ready 之后只读 GC 管理的固定槽；分支内不调用、不分配，不需要解码或额外保根。
  auto* globals = builder_.CreateLoad(
      pointer,
      ByteAddress(data, NativeMemoryLayout::kProgramGlobals + NativeMemoryLayout::kValuePayload));
  auto* values =
      builder_.CreateLoad(pointer, ByteAddress(globals, NativeMemoryLayout::kObjectNativePayload));
  const auto& type = source_.value_types.at(instruction.dst);
  Store(instruction.dst,
        ReadHeapValue(ByteAddress(values, instruction.index * sizeof(runtime::Value)), type), type);
  builder_.CreateBr(loaded);
  builder_.SetInsertPoint(initialize);
  // 惰性初始化、循环初始化与失败重试仍由状态所有者处理。
  PrepareRuntimeCall();
  auto helper = module_.getOrInsertFunction("vela_native_v2_initialize_global",
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
}

void FunctionEmitter::EmitGlobalStore(const ir::Instruction& instruction) {
  using runtime::NativeMemoryLayout;
  auto* pointer = builder_.getPtrTy();
  auto* data = ProgramData();
  auto* globals = builder_.CreateLoad(
      pointer,
      ByteAddress(data, NativeMemoryLayout::kProgramGlobals + NativeMemoryLayout::kValuePayload));
  auto* values =
      builder_.CreateLoad(pointer, ByteAddress(globals, NativeMemoryLayout::kObjectNativePayload));
  WriteHeapValue(ByteAddress(values, instruction.index * sizeof(runtime::Value)), instruction.lhs);
  EmitWriteBarrier(globals, instruction.lhs);
  // 赋值直接建立 Ready 状态，不执行原初始化器；写屏障仍由槽所属对象承担。
  auto* states =
      builder_.CreateLoad(pointer, ByteAddress(data, NativeMemoryLayout::kProgramGlobalStates));
  builder_.CreateStore(
      builder_.getInt8(static_cast<std::uint8_t>(runtime::NativeProgramState::GlobalState::Ready)),
      ByteAddress(states, instruction.index));
}

const layout::Type& FunctionEmitter::ArrayElement(const ir::Instruction& instruction) const {
  const auto array = program_.layouts.array_types.at(instruction.array_type);
  return program_.layouts.arrays.at(array).element;
}

llvm::Type* FunctionEmitter::ElementStorage(const layout::Type& element) {
  using ElementKind = layout::Type::Kind;
  if (element.kind == ElementKind::Dynamic ||
      (element.nullable &&
       (element.kind == ElementKind::Boolean || element.kind == ElementKind::Integer ||
        element.kind == ElementKind::Enum || element.kind == ElementKind::Double))) {
    return NativeRecordType(module_.getContext());
  }
  switch (element.kind) {
    case ElementKind::Boolean:
      return builder_.getInt8Ty();
    case ElementKind::Integer:
    case ElementKind::Enum:
      return builder_.getInt64Ty();
    case ElementKind::Double:
      return builder_.getDoubleTy();
    default:
      return builder_.getPtrTy();
  }
}

void FunctionEmitter::EmitIndex(const ir::Instruction& instruction) {
  using runtime::NativeMemoryLayout;
  const auto reading = instruction.opcode == Opcode::GetIndex;
  const auto array_reg = reading ? instruction.lhs : instruction.dst;
  auto* array = Reference(array_reg);
  auto* index = Payload(reading ? instruction.rhs : instruction.lhs);
  Check(builder_.CreateIsNotNull(array), ExecutionStatus::TypeError);
  auto* length =
      LoadMemory(builder_.getInt32Ty(), ByteAddress(array, NativeMemoryLayout::kObjectLength),
                 MemoryRegion::HeapHeader);
  // 无符号比较同时拒绝负数与超过 uint32 长度域的索引，保留精确失败位置。
  Check(builder_.CreateICmpULT(index, builder_.CreateZExt(length, builder_.getInt64Ty())),
        ExecutionStatus::IndexOutOfBounds);
  auto* data = LoadMemory(builder_.getPtrTy(), ByteAddress(array, NativeMemoryLayout::kArrayData),
                          MemoryRegion::HeapHeader);
  const auto& element = ArrayElement(instruction);
  auto* storage = ElementStorage(element);
  auto* address = builder_.CreateGEP(storage, data, index);
  const auto tagged = storage == NativeRecordType(module_.getContext());
  if (reading) {
    const auto& type = source_.value_types.at(instruction.dst);
    llvm::Value* value = nullptr;
    if (tagged) {
      value = ReadHeapValue(address, type);
    } else {
      auto* loaded = LoadMemory(storage, address, MemoryRegion::Elements);
      if (storage->isPointerTy() && !element.nullable) {
        // 构建中的引用槽只供初始化和 GC 使用；已交给脚本的数组满足元素的非空契约。
        // 使用实际元素布局，不能根据可能已擦除为 dynamic 的目标寄存器猜测空值形状。
        loaded->setMetadata(llvm::LLVMContext::MD_nonnull,
                            llvm::MDNode::get(module_.getContext(), {}));
      }
      if (storage == builder_.getInt64Ty() && array_reg < array_element_bits_.size()) {
        const auto bits = array_element_bits_.at(array_reg);
        if (bits != std::numeric_limits<std::uint64_t>::max()) {
          loaded->setMetadata(
              llvm::LLVMContext::MD_range,
              llvm::MDBuilder(module_.getContext())
                  .createRange(llvm::APInt(std::numeric_limits<std::uint64_t>::digits, 0),
                               llvm::APInt(std::numeric_limits<std::uint64_t>::digits, bits + 1)));
        }
      }
      value = loaded;
      if (storage == builder_.getInt8Ty()) {
        value = builder_.CreateICmpNE(value, builder_.getInt8(0));
      }
    }
    Store(instruction.dst, value, tagged ? type : ElementValueType(element));
  } else {
    WriteElement(address, element, instruction.rhs);
    EmitWriteBarrier(array, instruction.rhs);
  }
}

void FunctionEmitter::WriteElement(llvm::Value* address,
                                   const layout::Type& element,
                                   ir::RegisterId reg) {
  auto* storage = ElementStorage(element);
  if (storage == NativeRecordType(module_.getContext())) {
    WriteHeapValue(address, reg);
    return;
  }
  auto* value =
      Convert(builder_, Load(reg), source_.value_types.at(reg), ElementValueType(element));
  if (storage == builder_.getInt8Ty()) {
    value = builder_.CreateZExt(value, storage);
  }
  StoreMemory(value, address, MemoryRegion::Elements);
}

void FunctionEmitter::EmitArrayBuiltin(const ir::Instruction& instruction, Builtin builtin) {
  using runtime::NativeMemoryLayout;
  auto* array = Reference(instruction.lhs);
  Check(builder_.CreateIsNotNull(array), ExecutionStatus::TypeError);
  if (builtin == Builtin::ArrayCheckMutation) {
    auto* mutation =
        LoadMemory(builder_.getInt64Ty(), ByteAddress(array, NativeMemoryLayout::kArrayMutation),
                   MemoryRegion::HeapHeader);
    Check(builder_.CreateICmpEQ(mutation, Payload(instruction.lhs + 1)),
          ExecutionStatus::ConcurrentModification);
    Store(instruction.dst, builder_.getFalse(),
          {.kind = Kind::Null, .name = frontend::kNullTypeName});
    return;
  }
  if (builtin != Builtin::ArrayPush) {
    auto offset = NativeMemoryLayout::kObjectLength;
    if (builtin == Builtin::ArrayCapacity) {
      offset = NativeMemoryLayout::kArrayCapacity;
    } else if (builtin == Builtin::ArrayMutation) {
      offset = NativeMemoryLayout::kArrayMutation;
    }
    auto* type = builtin == Builtin::ArrayMutation ? builder_.getInt64Ty() : builder_.getInt32Ty();
    auto* value = LoadMemory(type, ByteAddress(array, offset), MemoryRegion::HeapHeader);
    Store(instruction.dst, builder_.CreateZExtOrTrunc(value, builder_.getInt64Ty()),
          {.kind = Kind::Int, .name = frontend::kIntTypeName});
    return;
  }
  auto* length_address = ByteAddress(array, NativeMemoryLayout::kObjectLength);
  auto* length = LoadMemory(builder_.getInt32Ty(), length_address, MemoryRegion::HeapHeader);
  auto* capacity =
      LoadMemory(builder_.getInt32Ty(), ByteAddress(array, NativeMemoryLayout::kArrayCapacity),
                 MemoryRegion::HeapHeader);
  auto* grow = llvm::BasicBlock::Create(module_.getContext(), "array.grow", body_);
  auto* append = llvm::BasicBlock::Create(module_.getContext(), "array.append", body_);
  auto* done = llvm::BasicBlock::Create(module_.getContext(), "array.done", body_);
  builder_.CreateCondBr(builder_.CreateICmpULT(length, capacity), append, grow);
  builder_.SetInsertPoint(grow);
  if (program_.native_mode == NativeProgramMode::Reloadable) {
    Poll();
  }
  // 只有扩容路径进入运行时；轮询可能改变数组，完整 push 必须重新读取当前长度。
  PrepareRuntimeCall();
  const auto reg = instruction.lhs + 1;
  PackArguments(instruction.lhs, BuiltinArity(builtin));
  auto helper = module_.getOrInsertFunction("vela_native_v1_builtin", builder_.getInt8Ty(),
                                            builder_.getPtrTy(), builder_.getInt32Ty(),
                                            builder_.getPtrTy(), builder_.getPtrTy());
  CompleteCall(builder_.CreateCall(helper,
                                   {
                                       body_->getArg(0),
                                       builder_.getInt32(instruction.index),
                                       arguments_,
                                       result_,
                                   }),
               instruction.dst);
  PollAfterRuntimeCall();
  builder_.CreateBr(done);
  builder_.SetInsertPoint(append);
  // 容量充足的分支没有运行时调用，元素缓冲区只在当前执行片段内借用。
  auto* data = LoadMemory(builder_.getPtrTy(), ByteAddress(array, NativeMemoryLayout::kArrayData),
                          MemoryRegion::HeapHeader);
  const auto& element = ArrayElement(instruction);
  auto* storage = ElementStorage(element);
  auto* address = builder_.CreateGEP(
      storage, data, builder_.CreateZExt(length, builder_.getInt64Ty()), "array.element");
  WriteElement(address, element, reg);
  auto* next_length = builder_.CreateNUWAdd(length, builder_.getInt32(1));
  StoreMemory(next_length, length_address, MemoryRegion::HeapHeader);
  auto* mutation_address = ByteAddress(array, NativeMemoryLayout::kArrayMutation);
  auto* mutation = LoadMemory(builder_.getInt64Ty(), mutation_address, MemoryRegion::HeapHeader);
  StoreMemory(builder_.CreateAdd(mutation, builder_.getInt64(1)), mutation_address,
              MemoryRegion::HeapHeader);
  EmitWriteBarrier(array, reg);
  Store(instruction.dst, builder_.CreateZExt(next_length, builder_.getInt64Ty()),
        {.kind = Kind::Int, .name = frontend::kIntTypeName});
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
}

}  // namespace vela::compiler::native
