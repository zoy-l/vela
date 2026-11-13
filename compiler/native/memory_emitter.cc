#include "base/native_program_mode.h"
#include "compiler/native/function_emitter.h"
#include "runtime/native/native_symbols.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

#include "base/builtin.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"
#include "compiler/native/array_analysis.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/field_access.h"
#include "compiler/native/memory_regions.h"
#include "compiler/native/native_values.h"
#include "runtime/heap/field_storage.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/instance_storage.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_memory_layout.h"
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
  DeclarationId field;
  if ((region == MemoryRegion::FieldPayloads || region == MemoryRegion::FieldKinds) &&
      IsClosedLinkedProgram(program_.native_mode, placement_)) {
    const auto& instruction = source_.instructions[instruction_index_];
    if (!instruction.field_declaration &&
        (instruction.opcode == Opcode::GetField || instruction.opcode == Opcode::SetField)) {
      const auto receiver =
          instruction.opcode == Opcode::GetField ? instruction.lhs : instruction.dst;
      auto& cached = closed_field_declarations_.at(instruction_index_);
      if (!cached) {
        cached =
            ClosedFieldDeclaration(program_, source_.value_types[receiver].name, instruction.index);
      }
      field = *cached;
    }
  }
  if (field == DeclarationId{}) {
    auto*& tag = memory_tags_.at(static_cast<std::size_t>(region));
    if (tag == nullptr) {
      tag = NativeMemoryTag(module_.getContext(), region);
    }
    return tag;
  }
  auto& tags = region == MemoryRegion::FieldPayloads ? field_payload_tags_ : field_kind_tags_;
  auto [entry, inserted] = tags.try_emplace(field.value, nullptr);
  if (inserted) {
    entry->second = NativeMemoryTag(module_.getContext(), region, field);
  }
  return entry->second;
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
  return {
      .payload = ByteAddress(address, NativeMemoryLayout::kValuePayload),
      .kind = ByteAddress(address, NativeMemoryLayout::kValueKind),
      .payload_region = region,
      .kind_region = region,
  };
}

FunctionEmitter::StoredValueAddress FunctionEmitter::FieldAddress(llvm::Value* storage,
                                                                  llvm::Value& index,
                                                                  llvm::Value* kind_offset) {
  using runtime::FieldStorage;
  auto* wide_index = builder_.CreateZExtOrTrunc(&index, builder_.getInt64Ty());
  auto* payload_offset =
      builder_.CreateMul(wide_index, builder_.getInt64(FieldStorage::kPayloadBytes));
  return {
      .payload = builder_.CreateGEP(builder_.getInt8Ty(), storage, payload_offset),
      .kind = builder_.CreateGEP(builder_.getInt8Ty(), storage, kind_offset),
      .payload_region = MemoryRegion::FieldPayloads,
      .kind_region = MemoryRegion::FieldKinds,
  };
}

FunctionEmitter::StoredValueAddress FunctionEmitter::ObjectFieldAddress(llvm::Value* object,
                                                                        std::uint32_t index) {
  const auto fields = LoadObjectFields(builder_, object, program_.native_mode);
  return {
      .payload = ByteAddress(fields.payloads, index * runtime::FieldStorage::kPayloadBytes),
      .kind = ByteAddress(fields.kinds, index * sizeof(runtime::ValueKind)),
      .payload_region = MemoryRegion::FieldPayloads,
      .kind_region = MemoryRegion::FieldKinds,
  };
}

FunctionEmitter::StoredValueAddress FunctionEmitter::DirectFieldAddress(llvm::Value* storage,
                                                                        std::uint32_t slot,
                                                                        std::uint32_t count) {
  using runtime::InstanceStorage;
  return FieldAddress(ByteAddress(storage, sizeof(InstanceStorage)), *builder_.getInt32(slot),
                      builder_.getInt64(runtime::FieldStorage::KindOffset(count, slot)));
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
  return MakeOptional(builder_, present, value, type);
}

FunctionEmitter::StoredValue FunctionEmitter::LoadStoredValue(ir::RegisterId reg) {
  return {.type = &source_.value_types.at(reg), .value = Load(reg), .encoded = nullptr};
}

llvm::Value* FunctionEmitter::EncodeStoredValue(StoredValue& value) {
  if (value.encoded == nullptr) {
    value.encoded = EncodeValue(builder_, value.value, *value.type);
  }
  return value.encoded;
}

void FunctionEmitter::WriteStoredValue(StoredValueAddress address,
                                       StoredValue& stored,
                                       FieldKindWrite kind_write) {
  const auto& type = *stored.type;
  auto* value = stored.value;
  auto* encoded = EncodeStoredValue(stored);
  if (IsReferenceKind(type.kind)) {
    // 用指针类型保留别名信息；不能将托管指针经整数运算后再写入对象。
    StoreMemory(value, address.payload, address.payload_region);
  } else {
    StoreMemory(builder_.CreateExtractValue(encoded, 0), address.payload, address.payload_region);
  }
  if (kind_write == FieldKindWrite::Required) {
    StoreMemory(builder_.CreateExtractValue(encoded, 1), address.kind, address.kind_region);
  }
}

void FunctionEmitter::EmitWriteBarrier(llvm::Value* object, StoredValue& stored) {
  using runtime::NativeMemoryLayout;
  const auto& type = *stored.type;
  const auto kind = type.kind;
  if (kind != Kind::Dynamic && !IsReferenceKind(kind)) {
    return;
  }
  auto* value = stored.value;
  auto* encoded = EncodeStoredValue(stored);
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
  MarkInvariant(*state);
  auto* phase =
      LoadMemory(builder_.getInt8Ty(), ByteAddress(state, offsetof(runtime::GcBarrierState, phase)),
                 MemoryRegion::GcState);
  auto* active = builder_.CreateICmpNE(
      phase, builder_.getInt8(static_cast<std::uint8_t>(runtime::GcPhase::Idle)));
  auto* inspect = llvm::BasicBlock::Create(module_.getContext(), "write.inspect", body_);
  auto* target = llvm::BasicBlock::Create(module_.getContext(), "write.target", body_);
  auto* barrier = llvm::BasicBlock::Create(module_.getContext(), "write.barrier", body_);
  auto* written = llvm::BasicBlock::Create(module_.getContext(), "write.done", body_);
  builder_.CreateCondBr(builder_.CreateAnd(reference, active), inspect, written,
                        llvm::MDBuilder(module_.getContext()).createUnlikelyBranchWeights());
  builder_.SetInsertPoint(inspect);
  // Idle 和非引用存储不访问对象标记位。
  auto* mark = LoadMemory(builder_.getInt8Ty(),
                          ByteAddress(state, offsetof(runtime::GcBarrierState, mark_bit)),
                          MemoryRegion::GcState);
  auto* source_mark =
      LoadMemory(builder_.getInt8Ty(), ByteAddress(object, NativeMemoryLayout::kObjectMarkBit),
                 MemoryRegion::HeapHeader);
  builder_.CreateCondBr(builder_.CreateICmpEQ(source_mark, mark), target, written,
                        llvm::MDBuilder(module_.getContext()).createUnlikelyBranchWeights());
  builder_.SetInsertPoint(target);
  // 非空引用守卫支配 header 读取；dynamic 可以携带 Object(nullptr) 或 String(nullptr)。
  // Mark 阶段的灰对象会继续扫描；Sweep 在返回脚本前已排空灰队列。
  auto* target_mark =
      LoadMemory(builder_.getInt8Ty(), ByteAddress(value, NativeMemoryLayout::kObjectMarkBit),
                 MemoryRegion::HeapHeader);
  builder_.CreateCondBr(builder_.CreateICmpNE(target_mark, mark), barrier, written,
                        llvm::MDBuilder(module_.getContext()).createUnlikelyBranchWeights());
  builder_.SetInsertPoint(barrier);
  auto helper =
      module_.getOrInsertFunction(runtime::kNativeWriteBarrierSymbol, builder_.getVoidTy(),
                                  builder_.getPtrTy(), builder_.getPtrTy(), builder_.getPtrTy());
  builder_.CreateCall(helper, {body_->getArg(0), object, Pack(encoded, 0)});
  builder_.CreateBr(written);
  builder_.SetInsertPoint(written);
}

void FunctionEmitter::EmitFieldAccess(const ir::Instruction& instruction,
                                      llvm::Value* object,
                                      const StoredValueAddress& address) {
  if (instruction.opcode == Opcode::GetField) {
    const auto& type = source_.value_types.at(instruction.dst);
    Store(instruction.dst, ReadStoredValue(address, type), type);
  } else {
    auto value = LoadStoredValue(instruction.lhs);
    WriteStoredValue(address, value, field_kind_writes_.at(instruction_index_));
    EmitWriteBarrier(object, value);
  }
}

void FunctionEmitter::EmitField(const ir::Instruction& instruction) {
  const auto reading = instruction.opcode == Opcode::GetField;
  auto* object = Reference(reading ? instruction.lhs : instruction.dst);
  CheckReference(object);
  if (instruction.field_declaration) {
    EmitReloadableField(instruction, object);
    return;
  }
  // 字段编号由静态成员解析确定，实际对象布局在入口边界验证；无需逐次查找布局。
  using runtime::NativeMemoryLayout;
  auto* count =
      LoadMemory(builder_.getInt32Ty(), ByteAddress(object, NativeMemoryLayout::kObjectFieldCount),
                 MemoryRegion::HeapHeader);
  MarkInvariant(*count);
  auto* kind_offset = builder_.CreateAdd(
      builder_.CreateMul(builder_.CreateZExt(count, builder_.getInt64Ty()),
                         builder_.getInt64(runtime::FieldStorage::kPayloadBytes)),
      builder_.getInt64(runtime::FieldStorage::KindOffset(0, instruction.index)));
  const auto address = FieldAddress(ByteAddress(object, sizeof(runtime::HeapObject)),
                                    *builder_.getInt32(instruction.index), kind_offset);
  EmitFieldAccess(instruction, object, address);
  if (!reading && program_.native_mode == NativeProgramMode::Reloadable && instruction.index == 0 &&
      source_.value_types.at(instruction.dst).kind == Kind::Function) {
    auto helper =
        module_.getOrInsertFunction(runtime::kNativeCaptureReceiverSymbol, builder_.getInt8Ty(),
                                    builder_.getPtrTy(), builder_.getPtrTy());
    CheckStatus(builder_.CreateCall(helper, {body_->getArg(0), object}));
  }
}

llvm::BasicBlock* FunctionEmitter::EmitFieldReadiness(llvm::Value* storage,
                                                      const ir::Instruction& instruction,
                                                      llvm::Value* slot,
                                                      std::optional<std::uint32_t> count,
                                                      llvm::BasicBlock* unavailable) {
  using runtime::FieldState;
  using runtime::InstanceStorage;
  auto& context = module_.getContext();
  auto* ready = llvm::BasicBlock::Create(context, "field.ready", body_);
  auto* inspect = llvm::BasicBlock::Create(context, "field.inspect", body_);
  auto* unready_address = ByteAddress(storage, offsetof(InstanceStorage, unready));
  auto* unready = LoadMemory(builder_.getInt32Ty(), unready_address, MemoryRegion::InstanceHeader);
  builder_.CreateCondBr(builder_.CreateICmpEQ(unready, builder_.getInt32(0)), ready, inspect,
                        llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(inspect);
  llvm::Value* physical_count = nullptr;
  if (count) {
    physical_count = builder_.getInt32(*count);
  } else {
    physical_count =
        LoadMemory(builder_.getInt32Ty(), ByteAddress(storage, offsetof(InstanceStorage, count)),
                   MemoryRegion::InstanceHeader);
  }
  auto* state_offset = builder_.CreateMul(
      builder_.CreateZExt(physical_count, builder_.getInt64Ty()),
      builder_.getInt64(runtime::FieldStorage::kPayloadBytes + runtime::FieldStorage::kKindBytes));
  auto* states = builder_.CreateGEP(builder_.getInt8Ty(),
                                    ByteAddress(storage, sizeof(InstanceStorage)), state_offset);
  auto* state_address = builder_.CreateGEP(builder_.getInt8Ty(), states,
                                           builder_.CreateZExt(slot, builder_.getInt64Ty()));
  auto* state = LoadMemory(builder_.getInt8Ty(), state_address, MemoryRegion::FieldStates);
  const auto reading = instruction.opcode == Opcode::GetField;
  auto* initialize =
      reading ? unavailable : llvm::BasicBlock::Create(context, "field.initialize", body_);
  builder_.CreateCondBr(
      builder_.CreateICmpEQ(state, builder_.getInt8(static_cast<std::uint8_t>(FieldState::Ready))),
      ready, initialize, llvm::MDBuilder(context).createLikelyBranchWeights());
  if (!reading) {
    builder_.SetInsertPoint(initialize);
    auto* available = llvm::BasicBlock::Create(context, "field.available", body_);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(
            state, builder_.getInt8(static_cast<std::uint8_t>(FieldState::Uninitialized))),
        available, unavailable, llvm::MDBuilder(context).createLikelyBranchWeights());
    builder_.SetInsertPoint(available);
    StoreMemory(builder_.CreateNUWSub(unready, builder_.getInt32(1)), unready_address,
                MemoryRegion::InstanceHeader);
    StoreMemory(builder_.getInt8(static_cast<std::uint8_t>(FieldState::Ready)), state_address,
                MemoryRegion::FieldStates);
    builder_.CreateBr(ready);
  }
  return ready;
}

void FunctionEmitter::EmitReloadableField(const ir::Instruction& instruction, llvm::Value* object) {
  using runtime::InstanceStorage;
  using runtime::NativeFieldCache;
  using runtime::NativeMemoryLayout;
  const auto reading = instruction.opcode == Opcode::GetField;
  const auto declaration = instruction.field_declaration;
  if (!declaration) {
    std::unreachable();
  }
  const auto field = *declaration;
  const auto origin = origins_[instruction_index_];
  const auto& canonical = CanonicalInstruction(origin);
  if (!reading &&
      (canonical.opcode != instruction.opcode || canonical.field_declaration != field)) {
    auto helper = module_.getOrInsertFunction(
        runtime::kNativeStoreMaterializedFieldSymbol, builder_.getInt8Ty(), builder_.getPtrTy(),
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
  const auto cached_access = field_accesses_.at(origin);
  if (!cached_access) {
    std::unreachable();
  }
  const auto access_index = *cached_access;
  auto* cache = ByteAddress(field_caches_, access_index * sizeof(NativeFieldCache));
  auto* layout =
      LoadMemory(builder_.getPtrTy(), ByteAddress(object, NativeMemoryLayout::kObjectLayout),
                 MemoryRegion::HeapHeader);
  auto* layout_id =
      LoadMemory(builder_.getInt64Ty(), ByteAddress(layout, NativeMemoryLayout::kClassId),
                 MemoryRegion::ClassLayout);
  MarkInvariant(*layout_id);
  auto* cached_id =
      LoadMemory(builder_.getInt64Ty(), ByteAddress(cache, offsetof(NativeFieldCache, layout_id)),
                 MemoryRegion::FieldCache);
  auto* matches = builder_.CreateICmpEQ(layout_id, cached_id);
  auto& context = module_.getContext();
  auto* cached = llvm::BasicBlock::Create(context, "field.cached", body_);
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
  builder_.SetInsertPoint(EmitFieldReadiness(storage, instruction, slot, std::nullopt, resolve));
  // 全部字段就绪后不再读取或写入逐字段状态，直到发布替换整个字段块。
  const auto address = FieldAddress(values, *slot, kind_offset);
  EmitFieldAccess(instruction, object, address);
  builder_.CreateBr(done);
  builder_.SetInsertPoint(resolve);
  if (reading) {
    PrepareInlineRuntimeCall();
  }
  auto helper = module_.getOrInsertFunction(
      reading ? runtime::kNativeLoadFieldSymbol : runtime::kNativeStoreFieldSymbol,
      builder_.getInt8Ty(), builder_.getPtrTy(), builder_.getPtrTy(), builder_.getInt64Ty(),
      builder_.getInt64Ty(), builder_.getPtrTy());
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
  using runtime::InstanceStorage;
  using runtime::NativeMemoryLayout;
  const auto declaration = instruction.field_declaration;
  if (!declaration) {
    std::unreachable();
  }
  const auto& direct = field_layouts_.at(*declaration);
  bool guarded_receiver = false;
  if (kind_ == NativeEntryKind::BoundedReflection) {
    if (!source_.signature) {
      std::unreachable();
    }
    const auto& signature = *source_.signature;
    if (!signature.receiver) {
      std::unreachable();
    }
    const auto receiver = *signature.receiver;
    guarded_receiver =
        field_targets_.at(*declaration).owner == program_.layouts.types[receiver].index;
  }
  if (guarded_receiver) {
    // 此入口仅由 CacheCall 在规范接收者布局下发布；调用点以唯一 layout id 守卫。
    // 包装器统一验证全部字段就绪；有界片段没有发布、分配或其他用户调用。
    auto* storage = LoadMemory(builder_.getPtrTy(),
                               ByteAddress(object, NativeMemoryLayout::kObjectNativePayload),
                               MemoryRegion::HeapHeader);
    const auto address = DirectFieldAddress(storage, direct.slot, direct.count);
    EmitFieldAccess(instruction, object, address);
    return;
  }
  auto& context = module_.getContext();
  auto* matched = llvm::BasicBlock::Create(context, "field.matched", body_);
  auto* recover = llvm::BasicBlock::Create(context, "field.recover", body_);
  auto* layout =
      LoadMemory(builder_.getPtrTy(), ByteAddress(object, NativeMemoryLayout::kObjectLayout),
                 MemoryRegion::HeapHeader);
  builder_.CreateCondBr(builder_.CreateICmpEQ(layout, direct.layout), matched, recover,
                        llvm::MDBuilder(context).createLikelyBranchWeights());
  builder_.SetInsertPoint(matched);
  auto* storage =
      LoadMemory(builder_.getPtrTy(), ByteAddress(object, NativeMemoryLayout::kObjectNativePayload),
                 MemoryRegion::HeapHeader);
  auto* ready = EmitFieldReadiness(storage, instruction, builder_.getInt32(direct.slot),
                                   direct.count, recover);
  builder_.SetInsertPoint(recover);
  // 不执行当前读写；保存规范输入后由基线处理布局变化、惰性初始化和精确异常。
  // 冷路径直接退出当前机器帧，成功路径间不再汇合可能修改堆存储的运行时调用。
  SetPosition();
  StoreRoots(instruction_index_);
  StoreCheckpoint();
  Recover();
  builder_.SetInsertPoint(ready);
  const auto address = DirectFieldAddress(storage, direct.slot, direct.count);
  EmitFieldAccess(instruction, object, address);
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

std::size_t FunctionEmitter::PlanFixedArrayStorage() {
  const auto count = static_cast<std::size_t>(std::ranges::count(array_access_.fixed_storage, 1));
  if (count == 0) {
    return 0;
  }
  const auto types = ArrayStorageTypes();
  fixed_array_cache_type_ = llvm::StructType::get(module_.getContext(), types);
  fixed_array_cache_bytes_ =
      module_.getDataLayout().getTypeAllocSize(fixed_array_cache_type_).getFixedValue();
  fixed_arrays_.resize(array_access_.fixed_storage.size());
  const auto alignment = runtime::kNativeFrameAlignment;
  return count * ((fixed_array_cache_bytes_ + alignment - 1) / alignment) * alignment;
}

void FunctionEmitter::CacheFixedArrayStorage() {
  for (std::size_t reg = 0; reg < array_access_.fixed_storage.size(); ++reg) {
    if (array_access_.fixed_storage[reg] != 0) {
      fixed_arrays_[reg] = AllocateAuxiliary(fixed_array_cache_bytes_);
    }
  }
}

void FunctionEmitter::StoreFixedArrayStorage(ir::RegisterId reg, llvm::Value* value) {
  // facts 按规范寄存器编号；逃逸改写可增加寄存器，动态入口也可以没有固定存储事实。
  if (reg >= fixed_arrays_.size() || fixed_arrays_[reg] == nullptr) {
    return;
  }
  auto* type = fixed_array_cache_type_;
  const auto& instruction = source_.instructions[instruction_index_];
  llvm::Value* record = nullptr;
  if (instruction.opcode == Opcode::Move) {
    // 固定组只含规范数组分配及 Move。逃逸改写新增的字段寄存器来自 GetField，
    // 该未知定义会使整个别名组失去 fixed_storage，不能成为这里的来源。
    record = builder_.CreateLoad(type, fixed_arrays_[instruction.lhs]);
  } else {
    // 只有新数组和 Move 能定义此别名组；数组不逃逸且不改变结构，GC 不移动其缓冲。
    record = llvm::PoisonValue::get(type);
    for (unsigned index = 0; index < type->getNumElements(); ++index) {
      record = builder_.CreateInsertValue(
          record, LoadArrayStorageField(value, static_cast<ArrayStorageField>(index)), index);
    }
  }
  builder_.CreateStore(record, fixed_arrays_[reg]);
}

llvm::Value* FunctionEmitter::ReadArrayStorage(llvm::Value* array,
                                               ir::RegisterId reg,
                                               ArrayStorageField field) {
  auto* type = fixed_array_cache_type_;
  const auto index = static_cast<unsigned>(field);
  if (reg < fixed_arrays_.size() && fixed_arrays_[reg] != nullptr) {
    return builder_.CreateLoad(type->getElementType(index),
                               builder_.CreateStructGEP(type, fixed_arrays_[reg], index));
  }
  return LoadArrayStorageField(array, field);
}

std::array<llvm::Type*, 3> FunctionEmitter::ArrayStorageTypes() {
  return {builder_.getPtrTy(), builder_.getInt32Ty(), builder_.getInt64Ty()};
}

llvm::Value* FunctionEmitter::LoadArrayStorageField(llvm::Value* array, ArrayStorageField field) {
  using runtime::NativeMemoryLayout;
  constexpr std::array kOffsets{
      NativeMemoryLayout::kArrayData,
      NativeMemoryLayout::kObjectLength,
      NativeMemoryLayout::kArrayMutation,
  };
  const auto types = ArrayStorageTypes();
  const auto index = static_cast<unsigned>(field);
  return LoadMemory(types[index], ByteAddress(array, kOffsets.at(index)), MemoryRegion::HeapHeader);
}

void FunctionEmitter::EmitIndex(const ir::Instruction& instruction) {
  using runtime::NativeMemoryLayout;
  const auto reading = instruction.opcode == Opcode::GetIndex;
  const auto array_reg = reading ? instruction.lhs : instruction.dst;
  auto* array = Reference(array_reg);
  auto* index = Payload(reading ? instruction.rhs : instruction.lhs);
  if (!HasRangeProof(array_access_.in_bounds)) {
    CheckReference(array);
    auto* length = ReadArrayStorage(array, array_reg, ArrayStorageField::Length);
    // 无符号比较同时拒绝负数与超过 uint32 长度域的索引，保留精确失败位置。
    Check(builder_.CreateICmpULT(index, builder_.CreateZExt(length, builder_.getInt64Ty())),
          ExecutionStatus::IndexOutOfBounds);
  }
  auto* data = ReadArrayStorage(array, array_reg, ArrayStorageField::Data);
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
      if (storage == builder_.getInt64Ty() && array_reg < array_access_.element_bits.size()) {
        const auto bits = array_access_.element_bits[array_reg];
        if (bits != kUnknownArrayElementBits) {
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
    auto value = LoadStoredValue(instruction.rhs);
    WriteElement(address, element, value);
    EmitWriteBarrier(array, value);
  }
}

void FunctionEmitter::WriteElement(llvm::Value* address,
                                   const layout::Type& element,
                                   StoredValue& stored) {
  auto* storage = ElementStorage(element);
  if (storage == NativeRecordType(module_.getContext())) {
    WriteStoredValue(RecordAddress(address, MemoryRegion::Values), stored);
    return;
  }
  auto* value = Convert(builder_, stored.value, *stored.type, ElementValueType(element));
  if (storage == builder_.getInt8Ty()) {
    value = builder_.CreateZExt(value, storage);
  }
  StoreMemory(value, address, MemoryRegion::Elements);
}

void FunctionEmitter::EmitArrayBuiltin(const ir::Instruction& instruction, Builtin builtin) {
  using runtime::NativeMemoryLayout;
  auto* array = Reference(instruction.lhs);
  CheckReference(array);
  if (builtin == Builtin::ArrayCheckMutation) {
    auto* mutation = ReadArrayStorage(array, instruction.lhs, ArrayStorageField::Mutation);
    Check(builder_.CreateICmpEQ(mutation, Payload(instruction.lhs + 1)),
          ExecutionStatus::ConcurrentModification);
    Store(instruction.dst, builder_.getFalse(),
          {.kind = Kind::Null, .name = frontend::kNullTypeName});
    return;
  }
  if (builtin != Builtin::ArrayPush) {
    llvm::Value* value = nullptr;
    if (builtin == Builtin::ArrayCapacity) {
      value =
          LoadMemory(builder_.getInt32Ty(), ByteAddress(array, NativeMemoryLayout::kArrayCapacity),
                     MemoryRegion::HeapHeader);
    } else {
      value = ReadArrayStorage(array, instruction.lhs,
                               builtin == Builtin::ArrayMutation ? ArrayStorageField::Mutation
                                                                 : ArrayStorageField::Length);
    }
    Store(instruction.dst, builder_.CreateZExtOrTrunc(value, builder_.getInt64Ty()),
          {.kind = Kind::Int, .name = frontend::kIntTypeName});
    return;
  }
  auto* length_address = ByteAddress(array, NativeMemoryLayout::kObjectLength);
  auto* length = LoadMemory(builder_.getInt32Ty(), length_address, MemoryRegion::HeapHeader);
  const auto reserved = HasRangeProof(array_access_.reserved_pushes);
  auto* append = llvm::BasicBlock::Create(module_.getContext(), "array.append", body_);
  auto* done = llvm::BasicBlock::Create(module_.getContext(), "array.done", body_);
  const auto reg = instruction.lhs + 1;
  if (reserved) {
    builder_.CreateBr(append);
  } else {
    auto* capacity =
        LoadMemory(builder_.getInt32Ty(), ByteAddress(array, NativeMemoryLayout::kArrayCapacity),
                   MemoryRegion::HeapHeader);
    auto* grow = llvm::BasicBlock::Create(module_.getContext(), "array.grow", body_);
    builder_.CreateCondBr(builder_.CreateICmpULT(length, capacity), append, grow);
    builder_.SetInsertPoint(grow);
    // 只有扩容路径进入运行时；轮询可能改变数组，完整 push 必须重新读取当前长度。
    PrepareInlineRuntimeCall();
    EmitRuntimeBuiltin(instruction);
    PollAfterRuntimeCall();
    builder_.CreateBr(done);
  }
  builder_.SetInsertPoint(append);
  // 容量充足的分支没有运行时调用，元素缓冲区只在当前执行片段内借用。
  auto* data = LoadMemory(builder_.getPtrTy(), ByteAddress(array, NativeMemoryLayout::kArrayData),
                          MemoryRegion::HeapHeader);
  const auto& element = ArrayElement(instruction);
  auto* storage = ElementStorage(element);
  auto* address = builder_.CreateGEP(
      storage, data, builder_.CreateZExt(length, builder_.getInt64Ty()), "array.element");
  auto value = LoadStoredValue(reg);
  WriteElement(address, element, value);
  auto* next_length = builder_.CreateNUWAdd(length, builder_.getInt32(1));
  StoreMemory(next_length, length_address, MemoryRegion::HeapHeader);
  auto* mutation_address = ByteAddress(array, NativeMemoryLayout::kArrayMutation);
  auto* mutation = LoadMemory(builder_.getInt64Ty(), mutation_address, MemoryRegion::HeapHeader);
  StoreMemory(builder_.CreateAdd(mutation, builder_.getInt64(1)), mutation_address,
              MemoryRegion::HeapHeader);
  EmitWriteBarrier(array, value);
  Store(instruction.dst, builder_.CreateZExt(next_length, builder_.getInt64Ty()),
        {.kind = Kind::Int, .name = frontend::kIntTypeName});
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
}

}  // namespace vela::compiler::native
