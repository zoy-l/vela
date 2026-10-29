#include "compiler/native/native_values.h"
#include "compiler/native/function_emitter.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>

#include "base/execution_status.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_image_emitter.h"
#include "runtime/native_abi.h"
#include "runtime/native_function.h"

namespace vela::compiler::native {

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
                               llvm::Type::getInt8Ty(context));
}

llvm::Value* LoadImport(Builder& builder,
                        llvm::Module& module,
                        llvm::Value* execution,
                        std::uint32_t function) {
  using runtime::NativeFunction;
  auto lookup = module.getOrInsertFunction("vela_native_v1_import", builder.getPtrTy(),
                                           builder.getPtrTy(), builder.getInt32Ty());
  auto* entry = builder.CreateCall(lookup, {execution, builder.getInt32(function)});
  auto address = [&](std::size_t offset) {
    return builder.CreateConstGEP1_64(builder.getInt8Ty(), entry, offset);
  };
  auto* slot =
      builder.CreateLoad(builder.getPtrTy(), address(offsetof(NativeFunction::AotEntry, slot)));
  auto* kind =
      builder.CreateLoad(builder.getInt8Ty(), address(offsetof(NativeFunction::AotEntry, kind)));
  // 槽类型只在此擦除，循环内只传实际入口和由 C++ 签名确定的调用约定。
  auto* target = builder.CreateLoad(builder.getPtrTy(), slot);
  llvm::Value* result = llvm::PoisonValue::get(NativeImportType(module.getContext()));
  result = builder.CreateInsertValue(result, target, 0);
  return builder.CreateInsertValue(result, kind, 1);
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
    return builder.CreateIsNotNull(value);
  }
  return builder.getInt1(type.kind != Kind::Null && type.kind != Kind::Void);
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
    auto* payload = HasValidity(type) ? builder.CreateExtractValue(value, 1) : value;
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
  auto* result = builder.CreateInsertValue(
      llvm::PoisonValue::get(ValueType(builder.getContext(), type)), present, 0);
  return builder.CreateInsertValue(result, payload, 1);
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
  auto* payload = HasValidity(source) ? builder.CreateExtractValue(value, 1) : value;
  if (!HasValidity(target)) {
    return payload;
  }
  auto* present = IsPresent(builder, value, source);
  auto* result = builder.CreateInsertValue(
      llvm::PoisonValue::get(ValueType(builder.getContext(), target)), present, 0);
  return builder.CreateInsertValue(result, payload, 1);
}

void EmitWrapper(llvm::Module& module,
                 const ir::Function& source,
                 std::uint32_t function_index,
                 const GeneratedFunction& function) {
  auto* body = function.body;
  auto* entry = function.entry;
  auto& context = module.getContext();
  Builder builder(llvm::BasicBlock::Create(context, "entry", entry));
  auto* invalid = llvm::BasicBlock::Create(context, "invalid", entry);
  auto* unpack = llvm::BasicBlock::Create(context, "unpack", entry);
  auto* count_matches =
      builder.CreateICmpEQ(entry->getArg(2), builder.getInt64(source.parameter_count));
  auto* valid = builder.CreateAnd(count_matches, builder.CreateIsNotNull(entry->getArg(3)));
  if (source.parameter_count != 0) {
    valid = builder.CreateAnd(valid, builder.CreateIsNotNull(entry->getArg(1)));
  }
  builder.CreateCondBr(valid, unpack, invalid);
  Builder invalid_builder(invalid);
  invalid_builder.CreateRet(Status(invalid_builder, ExecutionStatus::InvalidEntry));
  builder.SetInsertPoint(unpack);
  std::vector<llvm::Value*> arguments{entry->getArg(0)};
  for (std::size_t index = 0; index < source.parameter_count; ++index) {
    const auto& type = source.value_types.at(index);
    auto* address = builder.CreateConstGEP1_64(builder.getInt8Ty(), entry->getArg(1),
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
    auto* is_null = builder.CreateICmpEQ(kind, builder.getInt8(NativeKind(Kind::Null)));
    if (type.nullable) {
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
    auto* next = llvm::BasicBlock::Create(context, "argument", entry);
    auto* wrong_type = llvm::BasicBlock::Create(context, "type.error", entry);
    builder.CreateCondBr(matches, next, wrong_type);
    Builder wrong(wrong_type);
    wrong.CreateRet(Status(wrong, ExecutionStatus::TypeError));
    builder.SetInsertPoint(next);
    arguments.push_back(DecodeValue(builder, record, type));
  }
  auto* result_type = ValueType(context, source.return_type);
  auto* result = builder.CreateAlloca(result_type);
  if (!source.native_symbol.empty()) {
    auto* bound = llvm::BasicBlock::Create(context, "import.bound", entry);
    auto* no_execution = llvm::BasicBlock::Create(context, "invalid.isolate", entry);
    builder.CreateCondBr(builder.CreateIsNotNull(entry->getArg(0)), bound, no_execution);
    Builder missing(no_execution);
    missing.CreateRet(Status(missing, ExecutionStatus::InvalidIsolate));
    builder.SetInsertPoint(bound);
    arguments.push_back(LoadImport(builder, module, entry->getArg(0), function_index));
  }
  arguments.push_back(result);
  auto* status = builder.CreateCall(body, arguments);
  auto* returned = llvm::BasicBlock::Create(context, "returned", entry);
  auto* failed = llvm::BasicBlock::Create(context, "failed", entry);
  builder.CreateCondBr(builder.CreateICmpEQ(status, Status(builder, ExecutionStatus::Returned)),
                       returned, failed);
  Builder failure(failed);
  failure.CreateRet(status);
  builder.SetInsertPoint(returned);
  llvm::Value* value = builder.getFalse();
  if (source.return_type.kind != Kind::Void) {
    value = builder.CreateLoad(result_type, result);
  }
  builder.CreateStore(EncodeValue(builder, value, source.return_type), entry->getArg(3));
  builder.CreateRet(status);
}

}  // namespace vela::compiler::native
