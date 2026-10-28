#include "compiler/aot/function_emitter.h"

#include <cstddef>
#include <cstdint>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "base/opcode.h"
#include "compiler/aot/native_values.h"
#include "compiler/ir/ir.h"
#include "runtime/native_memory_layout.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::compiler::aot {
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
  return builder_.CreateConstGEP1_64(builder_.getInt8Ty(), object, offset);
}

llvm::Value* FunctionEmitter::ReadHeapValue(llvm::Value* address, const SourceType& type) {
  using runtime::NativeMemoryLayout;
  auto* payload = ByteAddress(address, NativeMemoryLayout::kValuePayload);
  if (type.kind == Kind::Dynamic) {
    llvm::Value* record = llvm::Constant::getNullValue(NativeRecordType(module_.getContext()));
    record =
        builder_.CreateInsertValue(record, builder_.CreateLoad(builder_.getInt64Ty(), payload), 0);
    auto* kind_value = builder_.CreateLoad(builder_.getInt8Ty(),
                                           ByteAddress(address, NativeMemoryLayout::kValueKind));
    return builder_.CreateInsertValue(record, kind_value, 1);
  }
  if (type.kind == Kind::Null || type.kind == Kind::Void) {
    return builder_.getFalse();
  }
  auto* storage_type =
      type.kind == Kind::Bool ? builder_.getInt8Ty() : PayloadType(module_.getContext(), type.kind);
  llvm::Value* value = builder_.CreateLoad(storage_type, payload);
  if (type.kind == Kind::Bool) {
    value = builder_.CreateICmpNE(value, builder_.getInt8(0));
  }
  if (!HasValidity(type)) {
    return value;
  }
  auto* tag = builder_.CreateLoad(builder_.getInt8Ty(),
                                  ByteAddress(address, NativeMemoryLayout::kValueKind));
  auto* present = builder_.CreateICmpNE(tag, builder_.getInt8(NativeKind(Kind::Null)));
  auto* result = builder_.CreateInsertValue(
      llvm::PoisonValue::get(ValueType(module_.getContext(), type)), present, 0);
  return builder_.CreateInsertValue(result, value, 1);
}

void FunctionEmitter::WriteHeapValue(llvm::Value* address, ir::RegisterId reg) {
  using runtime::NativeMemoryLayout;
  const auto& type = source_.value_types.at(reg);
  auto* encoded = Encode(reg);
  auto* payload = ByteAddress(address, NativeMemoryLayout::kValuePayload);
  if (IsReferenceKind(type.kind)) {
    // 用指针类型保留别名信息；不能将托管指针经整数运算后再写入对象。
    builder_.CreateStore(Load(reg), payload);
  } else {
    builder_.CreateStore(builder_.CreateExtractValue(encoded, 0), payload);
  }
  builder_.CreateStore(builder_.CreateExtractValue(encoded, 1),
                       ByteAddress(address, NativeMemoryLayout::kValueKind));
}

void FunctionEmitter::EmitWriteBarrier(llvm::Value* object, ir::RegisterId reg) {
  const auto kind = source_.value_types.at(reg).kind;
  if (kind != Kind::Dynamic && !IsReferenceKind(kind)) {
    return;
  }
  auto helper =
      module_.getOrInsertFunction("vela_native_v1_write_barrier", builder_.getVoidTy(),
                                  builder_.getPtrTy(), builder_.getPtrTy(), builder_.getPtrTy());
  builder_.CreateCall(helper, {body_->getArg(0), object, Pack(Encode(reg), 0)});
}

void FunctionEmitter::EmitField(const ir::Instruction& instruction) {
  const auto reading = instruction.opcode == Opcode::GetField;
  auto* object = Reference(reading ? instruction.lhs : instruction.dst);
  Check(builder_.CreateIsNotNull(object), ExecutionStatus::TypeError);
  // 字段编号由静态成员解析确定，实际对象布局在入口边界验证；无需逐次查找布局。
  auto* address = ByteAddress(
      object, sizeof(runtime::HeapObject) + (instruction.index * sizeof(runtime::Value)));
  if (reading) {
    const auto& type = source_.value_types.at(instruction.dst);
    Store(instruction.dst, ReadHeapValue(address, type), type);
  } else {
    WriteHeapValue(address, instruction.lhs);
    EmitWriteBarrier(object, instruction.lhs);
  }
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
  auto* length = builder_.CreateLoad(builder_.getInt32Ty(),
                                     ByteAddress(array, NativeMemoryLayout::kObjectLength));
  // 无符号比较同时拒绝负数与超过 uint32 长度域的索引，保留精确失败位置。
  Check(builder_.CreateICmpULT(index, builder_.CreateZExt(length, builder_.getInt64Ty())),
        ExecutionStatus::IndexOutOfBounds);
  auto* data =
      builder_.CreateLoad(builder_.getPtrTy(), ByteAddress(array, NativeMemoryLayout::kArrayData));
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
      value = builder_.CreateLoad(storage, address);
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
  builder_.CreateStore(value, address);
}

void FunctionEmitter::EmitArrayBuiltin(const ir::Instruction& instruction, Builtin builtin) {
  using runtime::NativeMemoryLayout;
  auto* array = Reference(instruction.lhs);
  Check(builder_.CreateIsNotNull(array), ExecutionStatus::TypeError);
  if (builtin != Builtin::ArrayPush) {
    auto offset = NativeMemoryLayout::kObjectLength;
    if (builtin == Builtin::ArrayCapacity) {
      offset = NativeMemoryLayout::kArrayCapacity;
    } else if (builtin == Builtin::ArrayMutation) {
      offset = NativeMemoryLayout::kArrayMutation;
    }
    auto* type = builtin == Builtin::ArrayMutation ? builder_.getInt64Ty() : builder_.getInt32Ty();
    auto* value = builder_.CreateLoad(type, ByteAddress(array, offset));
    Store(instruction.dst, builder_.CreateZExtOrTrunc(value, builder_.getInt64Ty()),
          {.kind = Kind::Int, .name = "int"});
    return;
  }
  auto* length_address = ByteAddress(array, NativeMemoryLayout::kObjectLength);
  auto* length = builder_.CreateLoad(builder_.getInt32Ty(), length_address);
  auto* capacity = builder_.CreateLoad(builder_.getInt32Ty(),
                                       ByteAddress(array, NativeMemoryLayout::kArrayCapacity));
  auto* grow = llvm::BasicBlock::Create(module_.getContext(), "array.grow", body_);
  auto* append = llvm::BasicBlock::Create(module_.getContext(), "array.append", body_);
  builder_.CreateCondBr(builder_.CreateICmpULT(length, capacity), append, grow);
  builder_.SetInsertPoint(grow);
  const auto reg = instruction.lhs + 1;
  auto helper =
      module_.getOrInsertFunction("vela_native_v1_array_grow", builder_.getInt8Ty(),
                                  builder_.getPtrTy(), builder_.getPtrTy(), builder_.getPtrTy());
  CheckStatus(builder_.CreateCall(helper, {body_->getArg(0), array, Pack(Encode(reg), 0)}));
  builder_.CreateBr(append);
  builder_.SetInsertPoint(append);
  // GC 不移动数组身份，扩容只迁移元素缓冲区；必须在 grow 后重新读取 data。
  auto* data =
      builder_.CreateLoad(builder_.getPtrTy(), ByteAddress(array, NativeMemoryLayout::kArrayData));
  const auto& element = ArrayElement(instruction);
  auto* storage = ElementStorage(element);
  auto* address = builder_.CreateGEP(
      storage, data, builder_.CreateZExt(length, builder_.getInt64Ty()), "array.element");
  WriteElement(address, element, reg);
  auto* next_length = builder_.CreateNUWAdd(length, builder_.getInt32(1));
  builder_.CreateStore(next_length, length_address);
  auto* mutation_address = ByteAddress(array, NativeMemoryLayout::kArrayMutation);
  auto* mutation = builder_.CreateLoad(builder_.getInt64Ty(), mutation_address);
  builder_.CreateStore(builder_.CreateAdd(mutation, builder_.getInt64(1)), mutation_address);
  EmitWriteBarrier(array, reg);
  Store(instruction.dst, builder_.CreateZExt(next_length, builder_.getInt64Ty()),
        {.kind = Kind::Int, .name = "int"});
}

}  // namespace vela::compiler::aot
