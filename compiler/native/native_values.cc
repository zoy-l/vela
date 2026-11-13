#include "compiler/native/native_values.h"
#include "compiler/native/function_emitter.h"
#include "runtime/native/native_symbols.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "base/reflection.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/memory_regions.h"
#include "runtime/heap/field_storage.h"
#include "runtime/heap/instance_storage.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {

std::optional<llvm::CmpInst::Predicate> IntegerComparison(Opcode opcode) {
  switch (opcode) {
    case Opcode::EqualInt:
      return llvm::CmpInst::ICMP_EQ;
    case Opcode::NotEqualInt:
      return llvm::CmpInst::ICMP_NE;
    case Opcode::LessInt:
      return llvm::CmpInst::ICMP_SLT;
    case Opcode::LessEqualInt:
      return llvm::CmpInst::ICMP_SLE;
    case Opcode::GreaterInt:
      return llvm::CmpInst::ICMP_SGT;
    case Opcode::GreaterEqualInt:
      return llvm::CmpInst::ICMP_SGE;
    default:
      return std::nullopt;
  }
}

void AssumeNonNull(Builder& builder, llvm::Value* value, const SourceType& type) {
  if (IsReferenceKind(type.kind) && !type.nullable) {
    // 装箱入口、运行时成功结果与已初始化全局都满足同一个类型非空契约。
    builder.CreateAssumption(builder.CreateIsNotNull(value));
  }
}

std::vector<std::optional<std::uint32_t>> FindStringConstants(
    const ir::Function& function,
    std::span<const ir::Function> functions) {
  const auto accesses = ir::AnalyzeRegisterAccesses(function, functions);
  const auto register_flow = ir::AnalyzeRegisterFlow(function, accesses);
  std::vector<std::optional<std::uint32_t>> result(function.register_count);
  for (ir::RegisterId reg = function.parameter_count; reg < function.register_count; ++reg) {
    const auto position = register_flow.OriginDefinition(reg);
    if (position) {
      const auto& definition = function.instructions[*position];
      if (definition.opcode == Opcode::LoadString) {
        result[reg] = definition.index;
      }
    }
  }
  return result;
}

llvm::Value* LoadProgramData(Builder& builder, llvm::Value* execution) {
  using runtime::NativeMemoryLayout;
  auto* instance_address = builder.CreateConstGEP1_64(builder.getInt8Ty(), execution,
                                                      NativeMemoryLayout::kExecutionInstance);
  auto* instance = builder.CreateLoad(builder.getPtrTy(), instance_address);
  instance->setMetadata(llvm::LLVMContext::MD_tbaa,
                        NativeMemoryTag(builder.getContext(), MemoryRegion::Execution));
  auto* data_address =
      builder.CreateConstGEP1_64(builder.getInt8Ty(), instance, NativeMemoryLayout::kInstanceData);
  auto* data = builder.CreateLoad(builder.getPtrTy(), data_address);
  data->setMetadata(llvm::LLVMContext::MD_tbaa,
                    NativeMemoryTag(builder.getContext(), MemoryRegion::InstanceHeader));
  return data;
}

llvm::Value* LoadProgramImage(Builder& builder, llvm::Value* execution) {
  auto* data = LoadProgramData(builder, execution);
  auto* address = builder.CreateConstGEP1_64(builder.getInt8Ty(), data,
                                             runtime::NativeMemoryLayout::kProgramImage);
  return builder.CreateLoad(builder.getPtrTy(), address);
}

NativeObjectFields LoadObjectFields(Builder& builder, llvm::Value* object, NativeProgramMode mode) {
  using runtime::NativeMemoryLayout;
  const auto address = [&](llvm::Value* base, std::size_t offset) {
    return builder.CreateConstGEP1_64(builder.getInt8Ty(), base, offset);
  };
  const auto load = [&](llvm::Type* type, llvm::Value* from, MemoryRegion region) {
    auto* value = builder.CreateLoad(type, from);
    value->setMetadata(llvm::LLVMContext::MD_tbaa, NativeMemoryTag(builder.getContext(), region));
    return value;
  };
  auto* inline_count = address(object, NativeMemoryLayout::kObjectFieldCount);
  llvm::Value* fields = address(object, sizeof(runtime::HeapObject));
  llvm::Value* count = nullptr;
  if (mode == NativeProgramMode::Closed) {
    auto* physical = load(builder.getInt32Ty(), inline_count, MemoryRegion::HeapHeader);
    MarkInvariant(*physical);
    count = physical;
  } else {
    auto* payload =
        load(builder.getPtrTy(), address(object, NativeMemoryLayout::kObjectNativePayload),
             MemoryRegion::HeapHeader);
    // 调用方已建立普通脚本实例契约，不含宿主对象或 callable 环境。
    // 此时 native_payload 仅在可重载实例中持有字段块；从物理存储读取字段数也支持派生类。
    auto* indirect = builder.CreateIsNotNull(payload);
    fields =
        builder.CreateSelect(indirect, address(payload, sizeof(runtime::InstanceStorage)), fields);
    auto* count_address = builder.CreateSelect(
        indirect, address(payload, offsetof(runtime::InstanceStorage, count)), inline_count);
    // 选择后的地址可属于任一区域，不能附加互斥的 header 标签。
    count = builder.CreateLoad(builder.getInt32Ty(), count_address);
  }
  auto* offset = builder.CreateMul(builder.CreateZExt(count, builder.getInt64Ty()),
                                   builder.getInt64(runtime::FieldStorage::kPayloadBytes));
  return {.payloads = fields, .kinds = builder.CreateGEP(builder.getInt8Ty(), fields, offset)};
}

llvm::Constant* EmitConstantTable(llvm::Module& module,
                                  llvm::StructType* table,
                                  llvm::Type* element,
                                  llvm::ArrayRef<llvm::Constant*> values,
                                  const std::string& name) {
  auto& context = module.getContext();
  llvm::Constant* data = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(context));
  if (!values.empty()) {
    auto* type = llvm::ArrayType::get(element, values.size());
    auto* contents = llvm::ConstantArray::get(type, values);
    auto* global = new llvm::GlobalVariable(module, type, true, llvm::GlobalValue::PrivateLinkage,
                                            contents, name);
    data = global;
  }
  return llvm::ConstantStruct::get(
      table, {data, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), values.size())});
}

bool IsScalar(const SourceType& type) {
  return type.kind == Kind::Int || type.kind == Kind::Enum || type.kind == Kind::Double ||
         type.kind == Kind::Bool || type.kind == Kind::Null || type.kind == Kind::Void;
}

bool IsReferenceKind(Kind kind) {
  return kind != Kind::Invalid && kind != Kind::Dynamic && !IsScalar({.kind = kind, .name = {}});
}

bool HasValidity(const SourceType& type) {
  return type.nullable && IsScalar(type) && type.kind != Kind::Null && type.kind != Kind::Void;
}

llvm::StructType* NativeRecordType(llvm::LLVMContext& context) {
  return llvm::StructType::get(llvm::Type::getInt64Ty(context), llvm::Type::getInt8Ty(context),
                               llvm::Type::getInt8Ty(context), llvm::Type::getInt16Ty(context),
                               llvm::Type::getInt32Ty(context));
}

llvm::StructType* NativeImportType(llvm::LLVMContext& context) {
  return llvm::StructType::get(llvm::PointerType::getUnqual(context),
                               llvm::Type::getInt8Ty(context), llvm::Type::getInt8Ty(context));
}

llvm::Value* LoadImport(Builder& builder,
                        llvm::Module& module,
                        llvm::Value* execution,
                        std::uint32_t function,
                        NativeCodePlacement placement) {
  using runtime::NativeFunction;
  auto lookup = module.getOrInsertFunction(runtime::kNativeImportSymbol, builder.getPtrTy(),
                                           builder.getPtrTy(), builder.getInt32Ty());
  auto* entry = builder.CreateCall(lookup, {execution, builder.getInt32(function)});
  auto address = [&](std::size_t offset) {
    return builder.CreateConstGEP1_64(builder.getInt8Ty(), entry, offset);
  };
  auto* slot =
      builder.CreateLoad(builder.getPtrTy(), address(offsetof(NativeFunction::AotEntry, slot)));
  llvm::Value* kind =
      builder.CreateLoad(builder.getInt8Ty(), address(offsetof(NativeFunction::AotEntry, kind)));
  auto* intrinsic = builder.CreateLoad(builder.getInt8Ty(),
                                       address(offsetof(NativeFunction::AotEntry, intrinsic)));
  llvm::Value* selected_slot = slot;
  if (placement == NativeCodePlacement::Linked) {
    auto* unscoped = builder.CreateLoad(builder.getPtrTy(),
                                        address(offsetof(NativeFunction::AotEntry, unscoped_slot)));
    auto* unscoped_kind = builder.CreateLoad(
        builder.getInt8Ty(), address(offsetof(NativeFunction::AotEntry, unscoped_kind)));
    auto* direct = builder.CreateICmpEQ(
        kind, builder.getInt8(static_cast<std::uint8_t>(NativeFunction::AotCallKind::Direct)));
    selected_slot = builder.CreateSelect(direct, unscoped, slot);
    kind = builder.CreateSelect(direct, unscoped_kind, kind);
  }
  // 槽类型只在此擦除，循环内只传实际入口和由 C++ 签名确定的调用约定。
  auto* target = builder.CreateLoad(builder.getPtrTy(), selected_slot);
  llvm::Value* result = llvm::PoisonValue::get(NativeImportType(module.getContext()));
  result = builder.CreateInsertValue(result, target, 0);
  result = builder.CreateInsertValue(result, kind, 1);
  return builder.CreateInsertValue(result, intrinsic, 2);
}

llvm::Type* PayloadType(llvm::LLVMContext& context, Kind kind) {
  if (kind == Kind::Double) {
    return llvm::Type::getDoubleTy(context);
  }
  if (kind == Kind::Dynamic) {
    return NativeRecordType(context);
  }
  if (IsReferenceKind(kind)) {
    return llvm::PointerType::getUnqual(context);
  }
  return kind == Kind::Bool || kind == Kind::Null || kind == Kind::Void
             ? llvm::Type::getInt1Ty(context)
             : llvm::Type::getInt64Ty(context);
}

llvm::Type* ValueType(llvm::LLVMContext& context, const SourceType& type) {
  auto* payload = PayloadType(context, type.kind);
  return HasValidity(type) ? llvm::StructType::get(llvm::Type::getInt1Ty(context), payload)
                           : payload;
}

llvm::ConstantInt* Status(Builder& builder, ExecutionStatus status) {
  return builder.getInt8(static_cast<std::uint8_t>(status));
}

void ReturnOnFailure(Builder& builder, llvm::Function& function, llvm::Value* status) {
  auto& context = function.getContext();
  auto* returned = llvm::BasicBlock::Create(context, "returned", &function);
  auto* failed = llvm::BasicBlock::Create(context, "failed", &function);
  builder.CreateCondBr(builder.CreateICmpEQ(status, Status(builder, ExecutionStatus::Returned)),
                       returned, failed);
  Builder failure(failed);
  failure.CreateRet(status);
  builder.SetInsertPoint(returned);
}

std::uint8_t NativeKind(Kind kind) {
  using runtime::NativeValueKind;
  switch (kind) {
    case Kind::Bool:
      return static_cast<std::uint8_t>(NativeValueKind::Boolean);
    case Kind::Int:
    case Kind::Enum:
      return static_cast<std::uint8_t>(NativeValueKind::Integer);
    case Kind::Double:
      return static_cast<std::uint8_t>(NativeValueKind::Double);
    case Kind::String:
      return static_cast<std::uint8_t>(NativeValueKind::String);
    default:
      return static_cast<std::uint8_t>(IsReferenceKind(kind) ? NativeValueKind::Object
                                                             : NativeValueKind::Null);
  }
}

llvm::Value* IsPresent(Builder& builder, llvm::Value* value, const SourceType& type) {
  if (HasValidity(type)) {
    return builder.CreateExtractValue(value, 0);
  }
  if (type.kind == Kind::Dynamic) {
    return builder.CreateICmpNE(builder.CreateExtractValue(value, 1),
                                builder.getInt8(NativeKind(Kind::Null)));
  }
  if (IsReferenceKind(type.kind)) {
    return type.nullable ? builder.CreateIsNotNull(value) : builder.getTrue();
  }
  return builder.getInt1(type.kind != Kind::Null && type.kind != Kind::Void);
}

llvm::Value* PayloadOf(Builder& builder, llvm::Value* value, const SourceType& type) {
  return HasValidity(type) ? builder.CreateExtractValue(value, 1) : value;
}

llvm::Value* MakeOptional(Builder& builder,
                          llvm::Value* present,
                          llvm::Value* payload,
                          const SourceType& type) {
  auto* result = builder.CreateInsertValue(
      llvm::PoisonValue::get(ValueType(builder.getContext(), type)), present, 0);
  return builder.CreateInsertValue(result, payload, 1);
}

void MarkInvariant(llvm::LoadInst& load) {
  load.setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(load.getContext(), {}));
}

llvm::Value* EncodeValue(Builder& builder, llvm::Value* value, const SourceType& type) {
  auto* representation = NativeRecordType(builder.getContext());
  llvm::Value* record = llvm::Constant::getNullValue(representation);
  if (type.kind == Kind::Null || type.kind == Kind::Void) {
    return record;
  }
  llvm::Value* bits = nullptr;
  llvm::Value* kind = nullptr;
  if (type.kind == Kind::Dynamic) {
    bits = builder.CreateExtractValue(value, 0);
    kind = builder.CreateExtractValue(value, 1);
  } else {
    auto* payload = PayloadOf(builder, value, type);
    if (IsReferenceKind(type.kind)) {
      bits = builder.CreatePtrToInt(payload, builder.getInt64Ty());
    } else if (type.kind == Kind::Double) {
      bits = builder.CreateBitCast(payload, builder.getInt64Ty());
    } else if (type.kind == Kind::Bool) {
      bits = builder.CreateZExt(payload, builder.getInt64Ty());
    } else {
      bits = payload;
    }
    auto* present = IsPresent(builder, value, type);
    bits = builder.CreateSelect(present, bits, builder.getInt64(0));
    kind = builder.CreateSelect(present, builder.getInt8(NativeKind(type.kind)),
                                builder.getInt8(NativeKind(Kind::Null)));
  }
  record = builder.CreateInsertValue(record, bits, 0);
  return builder.CreateInsertValue(record, kind, 1);
}

llvm::Value* DecodeValue(Builder& builder, llvm::Value* record, const SourceType& type) {
  if (type.kind == Kind::Dynamic) {
    return record;
  }
  if (type.kind == Kind::Null || type.kind == Kind::Void) {
    return builder.getFalse();
  }
  auto* bits = builder.CreateExtractValue(record, 0);
  auto* present = builder.CreateICmpNE(builder.CreateExtractValue(record, 1),
                                       builder.getInt8(NativeKind(Kind::Null)));
  llvm::Value* payload = bits;
  if (IsReferenceKind(type.kind)) {
    payload = builder.CreateIntToPtr(bits, builder.getPtrTy());
    return builder.CreateSelect(present, payload,
                                llvm::ConstantPointerNull::get(builder.getPtrTy()));
  }
  if (type.kind == Kind::Double) {
    payload = builder.CreateBitCast(bits, builder.getDoubleTy());
  } else if (type.kind == Kind::Bool) {
    payload = builder.CreateICmpNE(bits, builder.getInt64(0));
  }
  if (!HasValidity(type)) {
    return payload;
  }
  return MakeOptional(builder, present, payload, type);
}

llvm::Value* Convert(Builder& builder,
                     llvm::Value* value,
                     const SourceType& source,
                     const SourceType& target) {
  if (target.kind == Kind::Dynamic) {
    return EncodeValue(builder, value, source);
  }
  if (source.kind == Kind::Dynamic) {
    return DecodeValue(builder, value, target);
  }
  if (source.kind == Kind::Null || source.kind == Kind::Void) {
    return llvm::Constant::getNullValue(ValueType(builder.getContext(), target));
  }
  auto* payload = PayloadOf(builder, value, source);
  if (!HasValidity(target)) {
    return payload;
  }
  auto* present = IsPresent(builder, value, source);
  return MakeOptional(builder, present, payload, target);
}

void EmitExecutionBoundary(Builder& builder) {
  using runtime::NativeMemoryLayout;
  auto& context = builder.getContext();
  auto* function = builder.GetInsertBlock()->getParent();
  auto* execution = function->getArg(0);
  auto* valid = llvm::BasicBlock::Create(context, "execution.valid", function);
  auto* invalid = llvm::BasicBlock::Create(context, "execution.invalid", function);
  builder.CreateCondBr(builder.CreateIsNotNull(execution), valid, invalid,
                       llvm::MDBuilder(context).createLikelyBranchWeights());
  builder.SetInsertPoint(invalid);
  builder.CreateRet(Status(builder, ExecutionStatus::InvalidIsolate));
  builder.SetInsertPoint(valid);
  auto address = [&](std::size_t offset) {
    return builder.CreateConstGEP1_64(builder.getInt8Ty(), execution, offset);
  };
  // 构造时确定的线程栈状态只在真实入口验证，内部 typed 调用复用此契约。
  auto* stack_status =
      builder.CreateLoad(builder.getInt8Ty(), address(NativeMemoryLayout::kExecutionStackStatus));
  MarkInvariant(*stack_status);
  ReturnOnFailure(builder, *function, stack_status);
  auto* first = llvm::BasicBlock::Create(context, "execution.first", function);
  auto* ready = llvm::BasicBlock::Create(context, "execution.ready", function);
  auto* depth =
      builder.CreateLoad(builder.getInt64Ty(), address(NativeMemoryLayout::kExecutionDepth));
  auto* base_depth =
      builder.CreateLoad(builder.getInt64Ty(), address(NativeMemoryLayout::kExecutionBaseDepth));
  builder.CreateCondBr(builder.CreateICmpEQ(depth, base_depth), first, ready);
  builder.SetInsertPoint(first);
  // 宿主重入借用当前执行时必须保留外层 catch 的异常状态。
  builder.CreateStore(llvm::Constant::getNullValue(NativeRecordType(context)),
                      address(NativeMemoryLayout::kExecutionException));
  builder.CreateStore(builder.getInt8(0),
                      address(NativeMemoryLayout::kExecutionExceptionAllocationFailed));
  builder.CreateBr(ready);
  builder.SetInsertPoint(ready);
}

namespace {

using ReflectionRecord = std::pair<llvm::Value*, llvm::Value*>;

std::vector<ReflectionRecord> ReflectionRecords(Builder& builder,
                                                const ir::Program& program,
                                                ReflectionCallInputs inputs,
                                                std::size_t count) {
  using runtime::NativeMemoryLayout;
  auto& context = builder.getContext();
  auto* function = builder.GetInsertBlock()->getParent();
  auto* objects = llvm::BasicBlock::Create(context, "arguments.objects", function);
  auto* snapshots = llvm::BasicBlock::Create(context, "arguments.snapshots", function);
  auto* merged = llvm::BasicBlock::Create(context, "arguments.records", function);
  builder.CreateCondBr(builder.CreateIsNotNull(inputs.snapshots), snapshots, objects);
  builder.SetInsertPoint(objects);
  auto* data_address =
      builder.CreateConstGEP1_64(builder.getInt8Ty(), inputs.items, NativeMemoryLayout::kArrayData);
  auto* data = builder.CreateLoad(builder.getPtrTy(), data_address);
  data->setMetadata(llvm::LLVMContext::MD_tbaa, NativeMemoryTag(context, MemoryRegion::HeapHeader));
  const auto value_field = ClosedFieldDeclaration(program, reflection::kCallArgumentName, 0);
  const auto name_field = ClosedFieldDeclaration(program, reflection::kCallArgumentName, 1);
  std::vector<ReflectionRecord> object_records;
  object_records.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    auto* slot = builder.CreateConstGEP1_64(builder.getPtrTy(), data, index);
    auto* argument = builder.CreateLoad(builder.getPtrTy(), slot);
    argument->setMetadata(llvm::LLVMContext::MD_tbaa,
                          NativeMemoryTag(context, MemoryRegion::Elements));
    const auto fields = LoadObjectFields(builder, argument, program.native_mode);
    auto* kind = builder.CreateLoad(builder.getInt8Ty(), fields.kinds);
    kind->setMetadata(llvm::LLVMContext::MD_tbaa,
                      NativeMemoryTag(context, MemoryRegion::FieldKinds, value_field));
    auto* name_address =
        builder.CreateConstGEP1_64(builder.getInt8Ty(), fields.kinds, sizeof(runtime::ValueKind));
    auto* name = builder.CreateLoad(builder.getInt8Ty(), name_address);
    name->setMetadata(llvm::LLVMContext::MD_tbaa,
                      NativeMemoryTag(context, MemoryRegion::FieldKinds, name_field));
    auto* payload = builder.CreateLoad(builder.getInt64Ty(), fields.payloads);
    payload->setMetadata(llvm::LLVMContext::MD_tbaa,
                         NativeMemoryTag(context, MemoryRegion::FieldPayloads, value_field));
    auto* record = builder.CreateInsertValue(
        builder.CreateInsertValue(llvm::Constant::getNullValue(NativeRecordType(context)), payload,
                                  0),
        kind, 1);
    object_records.emplace_back(record, name);
  }
  builder.CreateBr(merged);
  builder.SetInsertPoint(snapshots);
  std::vector<ReflectionRecord> records;
  records.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    auto* address = builder.CreateConstGEP1_64(builder.getInt8Ty(), inputs.snapshots,
                                               index * 2 * sizeof(runtime::NativeValue));
    auto* record = builder.CreateLoad(NativeRecordType(context), address);
    record->setMetadata(llvm::LLVMContext::MD_tbaa, NativeMemoryTag(context, MemoryRegion::Values));
    auto* name_address =
        builder.CreateConstGEP1_64(builder.getInt8Ty(), address, sizeof(runtime::NativeValue));
    auto* name_kind_address = builder.CreateConstGEP1_64(builder.getInt8Ty(), name_address,
                                                         NativeMemoryLayout::kValueKind);
    auto* name = builder.CreateLoad(builder.getInt8Ty(), name_kind_address);
    name->setMetadata(llvm::LLVMContext::MD_tbaa, NativeMemoryTag(context, MemoryRegion::Values));
    records.emplace_back(record, name);
  }
  builder.CreateBr(merged);
  builder.SetInsertPoint(merged);
  for (std::size_t index = 0; index < count; ++index) {
    auto& [record, name] = records[index];
    const auto& [object_record, object_name] = object_records[index];
    auto* value = builder.CreatePHI(NativeRecordType(context), 2);
    value->addIncoming(record, snapshots);
    value->addIncoming(object_record, objects);
    auto* name_kind = builder.CreatePHI(builder.getInt8Ty(), 2);
    name_kind->addIncoming(name, snapshots);
    name_kind->addIncoming(object_name, objects);
    record = value;
    name = name_kind;
  }
  return records;
}

std::vector<llvm::Value*> ReflectionArguments(Builder& builder,
                                              llvm::Function& entry,
                                              const ir::Program& program,
                                              const ir::Function& source) {
  auto* miss = llvm::BasicBlock::Create(builder.getContext(), "arguments.unmatched", &entry);
  Builder unmatched(miss);
  unmatched.CreateRet(unmatched.getInt8(runtime::kNativeReflectionCallMiss));
  return UnpackReflectionArguments(builder, program, source,
                                   {
                                       .execution = entry.getArg(0),
                                       .receiver = entry.getArg(1),
                                       .items = entry.getArg(2),
                                       .snapshots = entry.getArg(3),
                                   },
                                   *miss);
}

}  // namespace

std::vector<llvm::Value*> UnpackReflectionArguments(Builder& builder,
                                                    const ir::Program& program,
                                                    const ir::Function& source,
                                                    ReflectionCallInputs inputs,
                                                    llvm::BasicBlock& unmatched) {
  auto& context = builder.getContext();
  if (!source.signature) {
    std::unreachable();
  }
  const auto hidden = source.signature->hidden_parameters;
  const auto records = ReflectionRecords(builder, program, inputs, source.parameter_count - hidden);
  auto* function = builder.GetInsertBlock()->getParent();
  std::vector<llvm::Value*> arguments{inputs.execution};
  for (std::size_t index = 0; index < source.parameter_count; ++index) {
    const auto& type = source.value_types[index];
    if (index < hidden) {
      auto* receiver = EncodeValue(builder, inputs.receiver, {.kind = Kind::Class, .name = {}});
      arguments.push_back(DecodeValue(builder, receiver, type));
      continue;
    }
    const auto& [record, name] = records[index - hidden];
    auto* kind = builder.CreateExtractValue(record, 1);
    llvm::Value* matches = type.kind == Kind::Dynamic
                               ? builder.getTrue()
                               : builder.CreateICmpEQ(kind, builder.getInt8(NativeKind(type.kind)));
    if (type.nullable) {
      matches = builder.CreateOr(
          matches, builder.CreateICmpEQ(kind, builder.getInt8(NativeKind(Kind::Null))));
    }
    matches = builder.CreateAnd(
        matches, builder.CreateICmpEQ(name, builder.getInt8(NativeKind(Kind::Null))));
    auto* next = llvm::BasicBlock::Create(context, "argument", function);
    builder.CreateCondBr(matches, next, &unmatched);
    builder.SetInsertPoint(next);
    arguments.push_back(DecodeValue(builder, record, type));
  }
  return arguments;
}

namespace {

std::vector<llvm::Value*> ExternalArguments(Builder& builder,
                                            llvm::Function& entry,
                                            const ir::Function& source) {
  auto& context = builder.getContext();
  auto* invalid = llvm::BasicBlock::Create(context, "invalid", &entry);
  auto* unpack = llvm::BasicBlock::Create(context, "unpack", &entry);
  auto* count_matches =
      builder.CreateICmpEQ(entry.getArg(2), builder.getInt64(source.parameter_count));
  auto* valid = builder.CreateAnd(count_matches, builder.CreateIsNotNull(entry.getArg(3)));
  if (source.parameter_count != 0) {
    valid = builder.CreateAnd(valid, builder.CreateIsNotNull(entry.getArg(1)));
  }
  builder.CreateCondBr(valid, unpack, invalid);
  Builder invalid_builder(invalid);
  invalid_builder.CreateRet(Status(invalid_builder, ExecutionStatus::InvalidEntry));
  builder.SetInsertPoint(unpack);
  EmitExecutionBoundary(builder);
  std::vector<llvm::Value*> arguments{entry.getArg(0)};
  for (std::size_t index = 0; index < source.parameter_count; ++index) {
    const auto& type = source.value_types.at(index);
    auto* address = builder.CreateConstGEP1_64(builder.getInt8Ty(), entry.getArg(1),
                                               index * sizeof(runtime::NativeValue));
    auto* record = builder.CreateLoad(NativeRecordType(context), address);
    auto* kind = builder.CreateExtractValue(record, 1);
    auto* bits = builder.CreateExtractValue(record, 0);
    llvm::Value* matches =
        type.kind == Kind::Dynamic
            ? builder.CreateICmpULE(
                  kind,
                  builder.getInt8(static_cast<std::uint8_t>(runtime::NativeValueKind::String)))
            : builder.CreateICmpEQ(kind, builder.getInt8(NativeKind(type.kind)));
    llvm::Value* is_null = nullptr;
    if (type.nullable) {
      is_null = builder.CreateICmpEQ(kind, builder.getInt8(NativeKind(Kind::Null)));
      matches = builder.CreateOr(matches, is_null);
    }
    if (IsReferenceKind(type.kind)) {
      auto* has_pointer = builder.CreateICmpNE(bits, builder.getInt64(0));
      matches = builder.CreateAnd(
          matches, type.nullable ? builder.CreateOr(is_null, has_pointer) : has_pointer);
    }
    if (type.kind == Kind::Bool || type.kind == Kind::Dynamic) {
      auto* is_boolean = builder.CreateICmpEQ(kind, builder.getInt8(NativeKind(Kind::Bool)));
      auto* canonical = builder.CreateOr(builder.CreateNot(is_boolean),
                                         builder.CreateICmpULE(bits, builder.getInt64(1)));
      matches = builder.CreateAnd(matches, canonical);
    }
    auto* next = llvm::BasicBlock::Create(context, "argument", &entry);
    auto* wrong_type = llvm::BasicBlock::Create(context, "type.error", &entry);
    builder.CreateCondBr(matches, next, wrong_type);
    Builder wrong(wrong_type);
    wrong.CreateRet(Status(wrong, ExecutionStatus::TypeError));
    builder.SetInsertPoint(next);
    arguments.push_back(DecodeValue(builder, record, type));
  }
  return arguments;
}

}  // namespace

void EmitWrapper(llvm::Module& module,
                 const ir::Program& program,
                 std::uint32_t function_index,
                 const GeneratedFunction& function,
                 llvm::Constant* image,
                 NativeCodePlacement placement,
                 NativeEntryContract contract) {
  const auto& source = program.functions[function_index];
  const auto external = contract == NativeEntryContract::External;
  const auto bounded = !external && function.bounded_reflection_body != nullptr;
  auto* body = bounded ? function.bounded_reflection_body : function.body;
  auto* entry = external ? function.entry : function.reflection_entry;
  auto& context = module.getContext();
  Builder builder(llvm::BasicBlock::Create(context, "entry", entry));
  if (bounded) {
    using runtime::NativeMemoryLayout;
    auto* miss = llvm::BasicBlock::Create(context, "bounded.unready", entry);
    Builder unmatched(miss);
    unmatched.CreateRet(unmatched.getInt8(runtime::kNativeReflectionCallMiss));
    // 调用点已验证本次边界没有请求并保存观察状态；有界体中没有成功的发布边界。
    // 初始化未完成时必须在任何副作用之前交还完整入口，不能执行后再重放。
    auto* storage_address = builder.CreateConstGEP1_64(builder.getInt8Ty(), entry->getArg(1),
                                                       NativeMemoryLayout::kObjectNativePayload);
    auto* storage = builder.CreateLoad(builder.getPtrTy(), storage_address);
    storage->setMetadata(llvm::LLVMContext::MD_tbaa,
                         NativeMemoryTag(context, MemoryRegion::HeapHeader));
    auto* unready_address = builder.CreateConstGEP1_64(builder.getInt8Ty(), storage,
                                                       offsetof(runtime::InstanceStorage, unready));
    auto* unready = builder.CreateLoad(builder.getInt32Ty(), unready_address);
    unready->setMetadata(llvm::LLVMContext::MD_tbaa,
                         NativeMemoryTag(context, MemoryRegion::InstanceHeader));
    auto* initialized = llvm::BasicBlock::Create(context, "bounded.initialized", entry);
    builder.CreateCondBr(builder.CreateICmpEQ(unready, builder.getInt32(0)), initialized, miss,
                         llvm::MDBuilder(context).createLikelyBranchWeights());
    builder.SetInsertPoint(initialized);
  }
  auto arguments = external ? ExternalArguments(builder, *entry, source)
                            : ReflectionArguments(builder, *entry, program, source);
  llvm::Value* current_image = image;
  if (external && function.baseline_body != nullptr) {
    current_image = LoadProgramImage(builder, entry->getArg(0));
  }
  auto* result_type = ValueType(context, source.return_type);
  // 实参检查会创建多个块；结果槽仍放在真实入口，供内联后的 SROA/SSA 提升，
  // 避免把固定大小结果误生成为每次调用都调整 SP 的动态栈分配。
  Builder allocation(&entry->getEntryBlock(), entry->getEntryBlock().begin());
  auto* result = allocation.CreateAlloca(result_type);
  if (!source.native_symbol.empty()) {
    arguments.push_back(LoadImport(builder, module, entry->getArg(0), function_index, placement));
  }
  arguments.push_back(result);
  llvm::Value* status = nullptr;
  if (external && function.baseline_body != nullptr) {
    auto helper = module.getOrInsertFunction(runtime::kNativeCanOptimizeSymbol, builder.getInt8Ty(),
                                             builder.getPtrTy(), builder.getPtrTy());
    auto* optimized = llvm::BasicBlock::Create(context, "optimized", entry);
    auto* baseline = llvm::BasicBlock::Create(context, "baseline", entry);
    auto* merge = llvm::BasicBlock::Create(context, "called", entry);
    auto* current = builder.CreateCall(helper, {entry->getArg(0), current_image});
    builder.CreateCondBr(builder.CreateICmpNE(current, builder.getInt8(0)), optimized, baseline);
    builder.SetInsertPoint(optimized);
    auto* fast = builder.CreateCall(body, arguments);
    builder.CreateBr(merge);
    builder.SetInsertPoint(baseline);
    auto* stable = builder.CreateCall(function.baseline_body, arguments);
    builder.CreateBr(merge);
    builder.SetInsertPoint(merge);
    auto* outcome = builder.CreatePHI(builder.getInt8Ty(), 2);
    outcome->addIncoming(fast, optimized);
    outcome->addIncoming(stable, baseline);
    status = outcome;
  } else {
    status = builder.CreateCall(body, arguments);
  }
  ReturnOnFailure(builder, *entry, status);
  llvm::Value* value = builder.getFalse();
  if (source.return_type.kind != Kind::Void) {
    value = builder.CreateLoad(result_type, result);
  }
  builder.CreateStore(EncodeValue(builder, value, source.return_type),
                      entry->getArg(external ? 3 : 4));
  builder.CreateRet(status);
}

}  // namespace vela::compiler::native
