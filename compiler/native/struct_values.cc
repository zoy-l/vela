#include "compiler/native/struct_values.h"

#include <absl/functional/function_ref.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Alignment.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "base/execution_status.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"
#include "runtime/heap/field_storage.h"
#include "runtime/heap/heap.h"
#include "runtime/native/native_symbols.h"

#include "compiler/native/native_values.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {

llvm::Value* ExtractStructField(Builder& builder,
                                llvm::Value* value,
                                const SourceType& owner,
                                std::uint32_t field) {
  const auto& shape = *owner.value_layout;
  const auto& type = shape.fields.at(field);
  auto offset = shape.offsets.at(field);
  auto* payload = PayloadOf(builder, value, owner);
  if (type.value_layout == nullptr) {
    return builder.CreateExtractValue(payload, offset);
  }
  auto field_type = type;
  field_type.nullable = false;
  llvm::Value* nested = llvm::Constant::getNullValue(ValueType(builder.getContext(), field_type));
  auto* present = type.nullable ? builder.CreateExtractValue(payload, offset++) : builder.getTrue();
  for (unsigned slot = 0; slot < type.value_layout->slots.size(); ++slot) {
    nested =
        builder.CreateInsertValue(nested, builder.CreateExtractValue(payload, offset + slot), slot);
  }
  return type.nullable ? MakeOptional(builder, present, nested, type) : nested;
}

llvm::Value* InsertStructField(Builder& builder,
                               llvm::Value* value,
                               const SourceType& owner,
                               std::uint32_t field,
                               llvm::Value* replacement) {
  const auto& shape = *owner.value_layout;
  const auto& type = shape.fields.at(field);
  auto offset = shape.offsets.at(field);
  auto* payload = PayloadOf(builder, value, owner);
  if (type.value_layout == nullptr) {
    payload = builder.CreateInsertValue(payload, replacement, offset);
  } else {
    auto* present = IsPresent(builder, replacement, type);
    if (type.nullable) {
      payload = builder.CreateInsertValue(payload, present, offset++);
    }
    auto* nested = PayloadOf(builder, replacement, type);
    if (type.nullable) {
      nested =
          builder.CreateSelect(present, nested, llvm::Constant::getNullValue(nested->getType()));
    }
    for (unsigned slot = 0; slot < type.value_layout->slots.size(); ++slot) {
      payload = builder.CreateInsertValue(payload, builder.CreateExtractValue(nested, slot),
                                          offset + slot);
    }
  }
  return owner.nullable ? MakeOptional(builder, IsPresent(builder, value, owner), payload, owner)
                        : payload;
}

std::size_t ManagedSlotCount(const SourceType& type) {
  if (type.value_layout == nullptr) {
    return type.kind == Kind::Dynamic || IsReferenceKind(type.kind) ? 1 : 0;
  }
  return static_cast<std::size_t>(std::ranges::count_if(
      type.value_layout->slots,
      [](const auto& slot) { return slot.kind == Kind::Dynamic || IsReferenceKind(slot.kind); }));
}

// 可空父值缺失时，展开后的必填引用叶子也为 null；擦除不能给空指针写引用标签。
// 该表示同时用于装箱、GC 根与写屏障，不改变逻辑字段的非空类型契约。
llvm::Value* EncodeStructSlot(Builder& builder, llvm::Value* value, const SourceType& type) {
  auto storage_type = type;
  if (IsReferenceKind(type.kind)) {
    storage_type.nullable = true;
  }
  return EncodeValue(builder, value, storage_type);
}

void ForEachStructReference(Builder& builder,
                            llvm::Value* value,
                            const SourceType& type,
                            absl::FunctionRef<void(const SourceType&, llvm::Value*)> visit) {
  auto* payload = PayloadOf(builder, value, type);
  auto* present = IsPresent(builder, value, type);
  const auto& slots = type.value_layout->slots;
  for (unsigned index = 0; index < slots.size(); ++index) {
    const auto& slot = slots[index];
    if (ManagedSlotCount(slot) == 0) {
      continue;
    }
    auto* encoded = EncodeStructSlot(builder, builder.CreateExtractValue(payload, index), slot);
    encoded = builder.CreateSelect(
        present, encoded, llvm::Constant::getNullValue(NativeRecordType(builder.getContext())));
    visit(slot, encoded);
  }
}

void StoreStructBoxFields(Builder& builder,
                          llvm::Value* object,
                          const SourceType& type,
                          llvm::Value* value) {
  auto* fields = LoadInlineObjectFields(builder, object);
  auto* payload = PayloadOf(builder, value, type);
  const auto& slots = type.value_layout->slots;
  const auto count = static_cast<std::uint32_t>(slots.size());
  for (unsigned index = 0; index < slots.size(); ++index) {
    auto* encoded =
        EncodeStructSlot(builder, builder.CreateExtractValue(payload, index), slots[index]);
    auto* address = builder.CreateConstGEP1_64(builder.getInt64Ty(), fields, index);
    builder.CreateStore(builder.CreateExtractValue(encoded, 0), address);
    auto* tag = builder.CreateConstGEP1_64(builder.getInt8Ty(), fields,
                                           runtime::FieldStorage::KindOffset(count, index));
    builder.CreateStore(builder.CreateExtractValue(encoded, 1), tag);
  }
}

namespace {

llvm::Function* StructBoxFunction(llvm::Module& module,
                                  const ir::Program& program,
                                  const SourceType& type) {
  const auto name = std::string("vela.struct.box.") + std::string(type.name);
  if (auto* existing = module.getFunction(name)) {
    return existing;
  }
  const auto owner =
      std::ranges::find(program.classes, frontend::NonNullableTypeName(type), &ir::Class::name);
  const auto class_index = static_cast<std::uint32_t>(owner - program.classes.begin());
  auto& context = module.getContext();
  auto* pointer = llvm::PointerType::getUnqual(context);
  auto* signature = llvm::FunctionType::get(llvm::Type::getInt8Ty(context),
                                            {pointer, ValueType(context, type), pointer}, false);
  auto* function =
      llvm::Function::Create(signature, llvm::GlobalValue::InternalLinkage, name, module);
  Builder builder(llvm::BasicBlock::Create(context, "entry", function));
  auto* value = function->getArg(1);
  auto* output = function->getArg(2);
  if (type.nullable) {
    auto* present = llvm::BasicBlock::Create(context, "present", function);
    auto* absent = llvm::BasicBlock::Create(context, "absent", function);
    builder.CreateCondBr(IsPresent(builder, value, type), present, absent);
    builder.SetInsertPoint(absent);
    builder.CreateStore(llvm::Constant::getNullValue(NativeRecordType(context)), output);
    builder.CreateRet(Status(builder, ExecutionStatus::Returned));
    builder.SetInsertPoint(present);
  }
  const auto root_count = ManagedSlotCount(type);
  llvm::Value* range = nullptr;
  if (root_count != 0) {
    Builder allocation(&function->getEntryBlock(), function->getEntryBlock().begin());
    auto* roots =
        allocation.CreateAlloca(llvm::ArrayType::get(NativeRecordType(context), root_count));
    auto* root_range = allocation.CreateAlloca(
        llvm::ArrayType::get(builder.getInt8Ty(), sizeof(runtime::RootRange)));
    root_range->setAlignment(llvm::Align(alignof(runtime::RootRange)));
    range = root_range;
    std::size_t root = 0;
    ForEachStructReference(builder, value, type, [&](const auto&, auto* encoded) {
      auto* address = builder.CreateConstGEP1_64(NativeRecordType(context), roots, root++);
      builder.CreateStore(encoded, address);
    });
    auto attach =
        module.getOrInsertFunction(runtime::kNativeRootsAttachSymbol, builder.getVoidTy(), pointer,
                                   pointer, builder.getInt64Ty(), builder.getInt64Ty(), pointer);
    builder.CreateCall(attach, {
                                   function->getArg(0),
                                   roots,
                                   builder.getInt64(root_count),
                                   builder.getInt64(sizeof(runtime::Value)),
                                   range,
                               });
  }
  auto allocate = module.getOrInsertFunction(runtime::kNativeNewObjectSymbol, pointer, pointer,
                                             builder.getInt32Ty());
  auto* object = builder.CreateCall(allocate, {function->getArg(0), builder.getInt32(class_index)});
  if (range != nullptr) {
    auto leave =
        module.getOrInsertFunction(runtime::kNativeRootsExitSymbol, builder.getVoidTy(), pointer);
    builder.CreateCall(leave, {range});
  }
  auto* ready = llvm::BasicBlock::Create(context, "ready", function);
  auto* failed = llvm::BasicBlock::Create(context, "failed", function);
  builder.CreateCondBr(builder.CreateIsNotNull(object), ready, failed);
  builder.SetInsertPoint(failed);
  builder.CreateRet(Status(builder, ExecutionStatus::OutOfMemory));
  builder.SetInsertPoint(ready);
  StoreStructBoxFields(builder, object, type, value);
  builder.CreateStore(EncodeValue(builder, object, {.kind = Kind::Class, .name = {}}), output);
  builder.CreateRet(Status(builder, ExecutionStatus::Returned));
  return function;
}

}  // namespace

llvm::Value* BoxStructValue(Builder& builder,
                            const ir::Program& program,
                            llvm::Value* execution,
                            llvm::Value* value,
                            const SourceType& type,
                            llvm::Value* output) {
  auto& module = *builder.GetInsertBlock()->getModule();
  auto* box = StructBoxFunction(module, program, type);
  return builder.CreateCall(box, {execution, value, output});
}

llvm::Value* UnboxStructValue(Builder& builder, llvm::Value* record, const SourceType& type) {
  auto& context = builder.getContext();
  auto* function = builder.GetInsertBlock()->getParent();
  auto* present = IsPresent(builder, record, {.kind = Kind::Dynamic, .name = {}});
  auto* ready = llvm::BasicBlock::Create(context, "struct.unbox", function);
  auto* absent = builder.GetInsertBlock();
  auto* done = llvm::BasicBlock::Create(context, "struct.unboxed", function);
  builder.CreateCondBr(present, ready, done);
  builder.SetInsertPoint(ready);
  auto* object = DecodeValue(builder, record, {.kind = Kind::Class, .name = {}});
  auto* fields = LoadInlineObjectFields(builder, object);
  auto required = type;
  required.nullable = false;
  auto* representation = ValueType(context, required);
  llvm::Value* payload = llvm::Constant::getNullValue(representation);
  const auto& slots = type.value_layout->slots;
  const auto count = static_cast<std::uint32_t>(slots.size());
  for (unsigned index = 0; index < slots.size(); ++index) {
    auto* address = builder.CreateConstGEP1_64(builder.getInt64Ty(), fields, index);
    auto* bits = builder.CreateLoad(builder.getInt64Ty(), address);
    auto* tag_address = builder.CreateConstGEP1_64(builder.getInt8Ty(), fields,
                                                   runtime::FieldStorage::KindOffset(count, index));
    auto* tag = builder.CreateLoad(builder.getInt8Ty(), tag_address);
    auto* encoded = builder.CreateInsertValue(
        builder.CreateInsertValue(llvm::Constant::getNullValue(NativeRecordType(context)), bits, 0),
        tag, 1);
    payload =
        builder.CreateInsertValue(payload, DecodeValue(builder, encoded, slots[index]), index);
  }
  builder.CreateBr(done);
  builder.SetInsertPoint(done);
  auto* merged = builder.CreatePHI(representation, 2);
  merged->addIncoming(llvm::Constant::getNullValue(representation), absent);
  merged->addIncoming(payload, ready);
  return type.nullable ? MakeOptional(builder, present, merged, type) : merged;
}

}  // namespace vela::compiler::native
