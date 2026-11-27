#include "base/declaration.h"
#include "compiler/native/native_values.h"
#include "compiler/native/struct_values.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Module.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/functional/function_ref.h>

#include "base/execution_status.h"
#include "runtime/heap/field_storage.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/native/native_symbols.h"

namespace vela::compiler::native {
namespace {

enum class PlaceStorage : std::uint8_t { Local, Box, Array, Global, BoxSlot, CaptureCell };
enum class PlaceAccess : std::uint8_t { Read, Write };

std::string AccessName(std::string_view storage,
                       const SourceType& type,
                       PlaceAccess access,
                       std::string_view field = {}) {
  auto name =
      std::string("vela.struct.place.") + std::string(storage) + "." + std::string(type.name);
  if (type.nullable) {
    name += ".nullable";
  }
  if (!field.empty()) {
    name += "." + std::string(field);
  }
  name += access == PlaceAccess::Read ? ".read" : ".write";
  return name;
}

struct BoxWrite final {
  llvm::Value* execution;
  llvm::Value* object;
  llvm::Value* value;
};

llvm::StructType* PlaceType(llvm::LLVMContext& context) {
  auto* pointer = llvm::PointerType::getUnqual(context);
  return llvm::StructType::get(context, {pointer, pointer, pointer});
}

llvm::FunctionType* AccessType(llvm::LLVMContext& context) {
  auto* pointer = llvm::PointerType::getUnqual(context);
  return llvm::FunctionType::get(llvm::Type::getInt8Ty(context), {pointer, pointer, pointer},
                                 false);
}

void EmitStructWriteBarriers(Builder& builder, const SourceType& type, const BoxWrite& write) {
  if (ManagedSlotCount(type) == 0) {
    return;
  }
  const auto [execution, object, value] = write;
  auto& module = *builder.GetInsertBlock()->getModule();
  Builder allocation(&builder.GetInsertBlock()->getParent()->getEntryBlock(),
                     builder.GetInsertBlock()->getParent()->getEntryBlock().begin());
  auto* temporary = allocation.CreateAlloca(NativeRecordType(builder.getContext()));
  auto barrier =
      module.getOrInsertFunction(runtime::kNativeWriteBarrierSymbol, builder.getVoidTy(),
                                 builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy());
  ForEachStructReference(builder, value, type, [&](const auto&, auto* encoded) {
    builder.CreateStore(encoded, temporary);
    builder.CreateCall(barrier, {execution, object, temporary});
  });
}

void StoreBox(Builder& builder, const SourceType& type, const BoxWrite& write) {
  StoreStructBoxFields(builder, write.object, type, write.value);
  EmitStructWriteBarriers(builder, type, write);
}

void EmitLocalAccess(Builder& builder,
                     llvm::Function* function,
                     const SourceType& type,
                     PlaceAccess access) {
  auto& context = builder.getContext();
  auto* representation = ValueType(context, type);
  auto* location = function->getArg(1);
  auto* buffer = function->getArg(2);
  auto* address = builder.CreateLoad(builder.getPtrTy(), location);
  if (access == PlaceAccess::Read) {
    builder.CreateStore(builder.CreateLoad(representation, address), buffer);
  } else {
    auto* value = builder.CreateLoad(representation, buffer);
    builder.CreateStore(value, address);
    if (ManagedSlotCount(type) != 0) {
      auto* roots = builder.CreateLoad(builder.getPtrTy(),
                                       builder.CreateConstGEP1_64(builder.getPtrTy(), location, 1));
      auto* refresh = llvm::BasicBlock::Create(context, "roots.refresh", function);
      auto* done = llvm::BasicBlock::Create(context, "done", function);
      builder.CreateCondBr(builder.CreateIsNotNull(roots), refresh, done);
      builder.SetInsertPoint(refresh);
      std::size_t root = 0;
      ForEachStructReference(builder, value, type, [&](const auto&, auto* encoded) {
        builder.CreateStore(encoded,
                            builder.CreateConstGEP1_64(NativeRecordType(context), roots, root++));
      });
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
    }
  }
}

void EmitArrayAccess(Builder& builder,
                     llvm::Function* function,
                     const SourceType& type,
                     PlaceAccess access) {
  auto& context = builder.getContext();
  auto& module = *function->getParent();
  auto* representation = ValueType(context, type);
  auto* location = function->getArg(1);
  auto* buffer = function->getArg(2);
  auto* array = builder.CreateLoad(builder.getPtrTy(), location);
  auto* index = builder.CreateLoad(builder.getInt64Ty(),
                                   builder.CreateConstGEP1_64(builder.getPtrTy(), location, 1));
  auto* length = builder.CreateLoad(
      builder.getInt32Ty(), builder.CreateConstGEP1_64(builder.getInt8Ty(), array,
                                                       runtime::NativeMemoryLayout::kObjectLength));
  auto* ready = llvm::BasicBlock::Create(context, "element.ready", function);
  auto* invalid = llvm::BasicBlock::Create(context, "element.invalid", function);
  builder.CreateCondBr(
      builder.CreateICmpULT(index, builder.CreateZExt(length, builder.getInt64Ty())), ready,
      invalid);
  builder.SetInsertPoint(invalid);
  builder.CreateRet(Status(builder, ExecutionStatus::IndexOutOfBounds));
  builder.SetInsertPoint(ready);
  auto* data = builder.CreateLoad(
      builder.getPtrTy(), builder.CreateConstGEP1_64(builder.getInt8Ty(), array,
                                                     runtime::NativeMemoryLayout::kArrayData));
  const auto stride =
      std::max<std::uint64_t>(1, module.getDataLayout().getTypeAllocSize(representation));
  auto* address = builder.CreateGEP(builder.getInt8Ty(), data,
                                    builder.CreateMul(index, builder.getInt64(stride)));
  if (access == PlaceAccess::Read) {
    builder.CreateStore(builder.CreateLoad(representation, address), buffer);
  } else {
    auto* value = builder.CreateLoad(representation, buffer);
    builder.CreateStore(value, address);
    EmitStructWriteBarriers(builder, type,
                            {.execution = function->getArg(0), .object = array, .value = value});
  }
}

void EmitBoxAccess(Builder& builder,
                   llvm::Function* function,
                   const SourceType& type,
                   PlaceStorage storage,
                   PlaceAccess access) {
  auto& context = builder.getContext();
  auto& module = *function->getParent();
  auto* representation = ValueType(context, type);
  auto* location = function->getArg(1);
  auto* buffer = function->getArg(2);
  if (storage == PlaceStorage::Global || storage == PlaceStorage::BoxSlot) {
    llvm::Value* record = location;
    if (storage == PlaceStorage::Global) {
      auto* program = builder.CreateLoad(builder.getPtrTy(), location);
      auto* slot = builder.CreateLoad(builder.getInt32Ty(),
                                      builder.CreateConstGEP1_64(builder.getPtrTy(), location, 1));
      record = builder.CreateAlloca(NativeRecordType(context));
      auto helper = module.getOrInsertFunction(
          runtime::kNativeLoadGlobalSymbol, builder.getInt8Ty(), builder.getPtrTy(),
          builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy());
      ReturnOnFailure(builder, *function,
                      builder.CreateCall(helper, {function->getArg(0), program, slot, record}));
    }
    auto* boxed = builder.CreateLoad(NativeRecordType(context), record);
    if (access == PlaceAccess::Read) {
      builder.CreateStore(UnboxStructValue(builder, boxed, type), buffer);
    } else {
      auto* object = DecodeValue(builder, boxed, {.kind = Kind::Class, .name = {}});
      StoreBox(builder, type,
               {
                   .execution = function->getArg(0),
                   .object = object,
                   .value = builder.CreateLoad(representation, buffer),
               });
    }
  } else {
    auto* object = storage == PlaceStorage::CaptureCell
                       ? static_cast<llvm::Value*>(builder.CreateLoad(
                             builder.getPtrTy(), LoadInlineObjectFields(builder, location)))
                       : location;
    if (access == PlaceAccess::Read) {
      auto* boxed =
          EncodeValue(builder, object, {.kind = Kind::Class, .name = {}, .nullable = true});
      builder.CreateStore(UnboxStructValue(builder, boxed, type), buffer);
    } else {
      StoreBox(builder, type,
               {
                   .execution = function->getArg(0),
                   .object = object,
                   .value = builder.CreateLoad(representation, buffer),
               });
    }
  }
}

std::string_view StorageName(PlaceStorage storage) {
  switch (storage) {
    case PlaceStorage::Local:
      return "local";
    case PlaceStorage::Box:
      return "box";
    case PlaceStorage::Array:
      return "array";
    case PlaceStorage::Global:
      return "global";
    case PlaceStorage::BoxSlot:
      return "box.slot";
    case PlaceStorage::CaptureCell:
      return "capture.cell";
  }
  std::unreachable();
}

llvm::Function* AccessFunction(llvm::Module& module,
                               const SourceType& type,
                               PlaceStorage storage,
                               PlaceAccess access) {
  const auto name = AccessName(StorageName(storage), type, access);
  if (auto* existing = module.getFunction(name)) {
    return existing;
  }
  auto& context = module.getContext();
  auto* function =
      llvm::Function::Create(AccessType(context), llvm::GlobalValue::InternalLinkage, name, module);
  Builder builder(llvm::BasicBlock::Create(context, "entry", function));
  if (storage == PlaceStorage::Local) {
    EmitLocalAccess(builder, function, type, access);
  } else if (storage == PlaceStorage::Array) {
    EmitArrayAccess(builder, function, type, access);
  } else {
    EmitBoxAccess(builder, function, type, storage, access);
  }
  builder.CreateRet(Status(builder, ExecutionStatus::Returned));
  return function;
}

llvm::Value* CreatePlace(Builder& builder,
                         llvm::Value* location,
                         absl::FunctionRef<llvm::Function*(PlaceAccess)> get_access) {
  auto& context = builder.getContext();
  Builder allocation(&builder.GetInsertBlock()->getParent()->getEntryBlock(),
                     builder.GetInsertBlock()->getParent()->getEntryBlock().begin());
  auto* descriptor = allocation.CreateAlloca(PlaceType(context));
  for (const auto access : {PlaceAccess::Read, PlaceAccess::Write}) {
    builder.CreateStore(get_access(access),
                        builder.CreateStructGEP(PlaceType(context), descriptor,
                                                access == PlaceAccess::Read ? 0 : 1));
  }
  builder.CreateStore(location, builder.CreateStructGEP(PlaceType(context), descriptor, 2));
  return descriptor;
}

llvm::Value* CreatePlace(Builder& builder,
                         llvm::Value* location,
                         const SourceType& type,
                         PlaceStorage storage) {
  auto& module = *builder.GetInsertBlock()->getModule();
  return CreatePlace(builder, location, [&](PlaceAccess access) {
    return AccessFunction(module, type, storage, access);
  });
}

struct PlaceTransfer final {
  llvm::Value* execution;
  llvm::Value* descriptor;
  llvm::Value* buffer;
};

llvm::Value* AccessPlace(Builder& builder, PlaceAccess access, const PlaceTransfer& transfer) {
  const auto [execution, descriptor, buffer] = transfer;
  auto& context = builder.getContext();
  auto* target = builder.CreateLoad(
      builder.getPtrTy(),
      builder.CreateStructGEP(PlaceType(context), descriptor, access == PlaceAccess::Read ? 0 : 1));
  auto* location = builder.CreateLoad(builder.getPtrTy(),
                                      builder.CreateStructGEP(PlaceType(context), descriptor, 2));
  return builder.CreateCall(AccessType(context), target, {execution, location, buffer});
}

struct FieldAddresses final {
  llvm::Value* payloads;
  llvm::Value* kinds;
};

FieldAddresses ResolveFieldAddresses(Builder& body,
                                     llvm::Function& function,
                                     std::uint32_t slot,
                                     std::optional<DeclarationId> field) {
  auto& module = *function.getParent();
  auto& context = body.getContext();
  auto* owner = function.getArg(1);
  if (field) {
    auto* storage =
        body.CreateAlloca(llvm::StructType::get(context, {body.getPtrTy(), body.getPtrTy()}));
    auto helper = module.getOrInsertFunction(runtime::kNativeBorrowStructFieldSymbol,
                                             body.getInt8Ty(), body.getPtrTy(), body.getPtrTy(),
                                             body.getInt64Ty(), body.getPtrTy());
    ReturnOnFailure(
        body, function,
        body.CreateCall(helper, {function.getArg(0), owner, body.getInt64(field->value), storage}));
    return {
        .payloads = body.CreateLoad(body.getPtrTy(), storage),
        .kinds =
            body.CreateLoad(body.getPtrTy(), body.CreateConstGEP1_64(body.getPtrTy(), storage, 1)),
    };
  }
  auto* fields = LoadInlineObjectFields(body, owner);
  auto* count = body.CreateLoad(
      body.getInt32Ty(), body.CreateConstGEP1_64(body.getInt8Ty(), owner,
                                                 runtime::NativeMemoryLayout::kObjectFieldCount));
  auto* offset = body.CreateAdd(body.CreateMul(body.CreateZExt(count, body.getInt64Ty()),
                                               body.getInt64(runtime::FieldStorage::kPayloadBytes)),
                                body.getInt64(slot));
  return {
      .payloads = body.CreateConstGEP1_64(body.getInt64Ty(), fields, slot),
      .kinds = body.CreateGEP(body.getInt8Ty(), fields, offset),
  };
}

void EmitObjectFieldAccess(Builder& body,
                           llvm::Function& function,
                           const SourceType& type,
                           PlaceAccess access,
                           const FieldAddresses& addresses) {
  auto& context = body.getContext();
  const auto [payloads, kinds] = addresses;
  auto required = type;
  required.nullable = false;
  auto* representation = ValueType(context, required);
  llvm::Value* value = llvm::Constant::getNullValue(representation);
  llvm::Value* present = body.getTrue();
  auto* source = access == PlaceAccess::Write
                     ? body.CreateLoad(ValueType(context, type), function.getArg(2))
                     : nullptr;
  unsigned offset = 0;
  if (type.nullable) {
    offset = 1;
    if (access == PlaceAccess::Read) {
      present = body.CreateICmpNE(body.CreateLoad(body.getInt64Ty(), payloads), body.getInt64(0));
    } else {
      present = IsPresent(body, source, type);
      body.CreateStore(body.CreateZExt(present, body.getInt64Ty()), payloads);
      body.CreateStore(body.getInt8(NativeKind(Kind::Bool)), kinds);
    }
  }
  llvm::Value* input = nullptr;
  if (access == PlaceAccess::Write) {
    input = PayloadOf(body, source, type);
    input = body.CreateSelect(present, input, llvm::Constant::getNullValue(representation));
  }
  const auto& slots = type.value_layout->slots;
  for (unsigned index = 0; index < slots.size(); ++index) {
    auto* payload_address = body.CreateConstGEP1_64(body.getInt64Ty(), payloads, offset + index);
    auto* kind_address = body.CreateConstGEP1_64(body.getInt8Ty(), kinds, offset + index);
    if (access == PlaceAccess::Read) {
      llvm::Value* record = llvm::Constant::getNullValue(NativeRecordType(context));
      record =
          body.CreateInsertValue(record, body.CreateLoad(body.getInt64Ty(), payload_address), 0);
      record = body.CreateInsertValue(record, body.CreateLoad(body.getInt8Ty(), kind_address), 1);
      value = body.CreateInsertValue(value, DecodeValue(body, record, slots[index]), index);
    } else {
      auto* encoded = EncodeStructSlot(body, body.CreateExtractValue(input, index), slots[index]);
      body.CreateStore(body.CreateExtractValue(encoded, 0), payload_address);
      body.CreateStore(body.CreateExtractValue(encoded, 1), kind_address);
    }
  }
  if (access == PlaceAccess::Read) {
    body.CreateStore(type.nullable ? MakeOptional(body, present, value, type) : value,
                     function.getArg(2));
  } else {
    EmitStructWriteBarriers(
        body, type,
        {.execution = function.getArg(0), .object = function.getArg(1), .value = source});
  }
}

}  // namespace

llvm::Value* LocalStructPlace(Builder& builder,
                              llvm::Value* storage,
                              llvm::Value* roots,
                              const SourceType& type) {
  Builder allocation(&builder.GetInsertBlock()->getParent()->getEntryBlock(),
                     builder.GetInsertBlock()->getParent()->getEntryBlock().begin());
  auto* location = allocation.CreateAlloca(llvm::ArrayType::get(builder.getPtrTy(), 2));
  builder.CreateStore(storage, location);
  builder.CreateStore(roots, builder.CreateConstGEP1_64(builder.getPtrTy(), location, 1));
  return CreatePlace(builder, location, type, PlaceStorage::Local);
}

llvm::Value* BoxedStructPlace(Builder& builder, llvm::Value* object, const SourceType& type) {
  return CreatePlace(builder, object, type, PlaceStorage::Box);
}

llvm::Value* CapturedStructPlace(Builder& builder, llvm::Value* cell, const SourceType& type) {
  return CreatePlace(builder, cell, type, PlaceStorage::CaptureCell);
}

llvm::Value* BoxSlotStructPlace(Builder& builder, llvm::Value* storage, const SourceType& type) {
  return CreatePlace(builder, storage, type, PlaceStorage::BoxSlot);
}

llvm::Value* ObjectFieldStructPlace(Builder& builder,
                                    llvm::Value* object,
                                    const SourceType& type,
                                    std::uint32_t slot,
                                    std::optional<DeclarationId> field) {
  auto& module = *builder.GetInsertBlock()->getModule();
  auto& context = builder.getContext();
  return CreatePlace(builder, object, [&](PlaceAccess access) {
    const auto name = AccessName(
        "object", type, access,
        field ? "declaration." + std::to_string(field->value) : "slot." + std::to_string(slot));
    auto* function = module.getFunction(name);
    if (function == nullptr) {
      function = llvm::Function::Create(AccessType(context), llvm::GlobalValue::InternalLinkage,
                                        name, module);
      Builder body(llvm::BasicBlock::Create(context, "entry", function));
      const auto addresses = ResolveFieldAddresses(body, *function, slot, field);
      EmitObjectFieldAccess(body, *function, type, access, addresses);
      body.CreateRet(Status(body, ExecutionStatus::Returned));
    }
    return function;
  });
}

llvm::Value* GlobalStructPlace(Builder& builder,
                               llvm::Value* program,
                               std::uint32_t slot,
                               const SourceType& type) {
  Builder allocation(&builder.GetInsertBlock()->getParent()->getEntryBlock(),
                     builder.GetInsertBlock()->getParent()->getEntryBlock().begin());
  auto* location = allocation.CreateAlloca(llvm::ArrayType::get(builder.getPtrTy(), 2));
  builder.CreateStore(program, location);
  builder.CreateStore(builder.getInt32(slot),
                      builder.CreateConstGEP1_64(builder.getPtrTy(), location, 1));
  return CreatePlace(builder, location, type, PlaceStorage::Global);
}

llvm::Value* ArrayStructPlace(Builder& builder,
                              llvm::Value* array,
                              llvm::Value* index,
                              const SourceType& type) {
  Builder allocation(&builder.GetInsertBlock()->getParent()->getEntryBlock(),
                     builder.GetInsertBlock()->getParent()->getEntryBlock().begin());
  auto* context = allocation.CreateAlloca(llvm::ArrayType::get(builder.getPtrTy(), 2));
  builder.CreateStore(array, context);
  builder.CreateStore(index, builder.CreateConstGEP1_64(builder.getPtrTy(), context, 1));
  return CreatePlace(builder, context, type, PlaceStorage::Array);
}

llvm::Value* ProjectStructPlace(Builder& builder,
                                llvm::Value* parent,
                                const SourceType& type,
                                std::uint32_t field) {
  auto& module = *builder.GetInsertBlock()->getModule();
  auto& context = builder.getContext();
  return CreatePlace(builder, parent, [&](PlaceAccess access) {
    const auto name = AccessName("field", type, access, std::to_string(field));
    auto* function = module.getFunction(name);
    if (function == nullptr) {
      function = llvm::Function::Create(AccessType(context), llvm::GlobalValue::InternalLinkage,
                                        name, module);
      Builder body(llvm::BasicBlock::Create(context, "entry", function));
      auto* value = body.CreateAlloca(ValueType(context, type));
      ReturnOnFailure(body, *function,
                      ReadStructPlace(body, function->getArg(0), function->getArg(1), value));
      if (access == PlaceAccess::Read) {
        auto* loaded = body.CreateLoad(ValueType(context, type), value);
        body.CreateStore(ExtractStructField(body, loaded, type, field), function->getArg(2));
      } else {
        const auto& field_type = type.value_layout->fields.at(field);
        auto* replacement = body.CreateLoad(ValueType(context, field_type), function->getArg(2));
        auto* loaded = body.CreateLoad(ValueType(context, type), value);
        body.CreateStore(InsertStructField(body, loaded, type, field, replacement), value);
        ReturnOnFailure(body, *function,
                        WriteStructPlace(body, function->getArg(0), function->getArg(1), value));
      }
      body.CreateRet(Status(body, ExecutionStatus::Returned));
    }
    return function;
  });
}

llvm::Value* ReadStructPlace(Builder& builder,
                             llvm::Value* execution,
                             llvm::Value* place,
                             llvm::Value* output) {
  return AccessPlace(builder, PlaceAccess::Read,
                     {.execution = execution, .descriptor = place, .buffer = output});
}

llvm::Value* RequiredStructPlace(Builder& builder, llvm::Value* parent, const SourceType& type) {
  auto& module = *builder.GetInsertBlock()->getModule();
  auto& context = builder.getContext();
  return CreatePlace(builder, parent, [&](PlaceAccess access) {
    const auto name = AccessName("required", type, access);
    auto* function = module.getFunction(name);
    if (function == nullptr) {
      function = llvm::Function::Create(AccessType(context), llvm::GlobalValue::InternalLinkage,
                                        name, module);
      Builder body(llvm::BasicBlock::Create(context, "entry", function));
      auto* value = body.CreateAlloca(ValueType(context, type));
      ReturnOnFailure(body, *function,
                      ReadStructPlace(body, function->getArg(0), function->getArg(1), value));
      auto* loaded = body.CreateLoad(ValueType(context, type), value);
      auto* ready = llvm::BasicBlock::Create(context, "present", function);
      auto* absent = llvm::BasicBlock::Create(context, "absent", function);
      body.CreateCondBr(IsPresent(body, loaded, type), ready, absent);
      body.SetInsertPoint(absent);
      body.CreateRet(Status(body, ExecutionStatus::TypeError));
      body.SetInsertPoint(ready);
      if (access == PlaceAccess::Read) {
        body.CreateStore(PayloadOf(body, loaded, type), function->getArg(2));
      } else {
        auto required = type;
        required.nullable = false;
        auto* replacement = body.CreateLoad(ValueType(context, required), function->getArg(2));
        body.CreateStore(MakeOptional(body, body.getTrue(), replacement, type), value);
        ReturnOnFailure(body, *function,
                        WriteStructPlace(body, function->getArg(0), function->getArg(1), value));
      }
      body.CreateRet(Status(body, ExecutionStatus::Returned));
    }
    return function;
  });
}

llvm::Value* WriteStructPlace(Builder& builder,
                              llvm::Value* execution,
                              llvm::Value* place,
                              llvm::Value* input) {
  return AccessPlace(builder, PlaceAccess::Write,
                     {.execution = execution, .descriptor = place, .buffer = input});
}

}  // namespace vela::compiler::native
