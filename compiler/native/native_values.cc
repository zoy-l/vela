#include "compiler/native/native_values.h"
#include "compiler/native/function_emitter.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include "base/execution_status.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/ir.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_memory_layout.h"

namespace vela::compiler::native {

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
  auto* intrinsic = builder.CreateLoad(builder.getInt8Ty(),
                                       address(offsetof(NativeFunction::AotEntry, intrinsic)));
  // 槽类型只在此擦除，循环内只传实际入口和由 C++ 签名确定的调用约定。
  auto* target = builder.CreateLoad(builder.getPtrTy(), slot);
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
  stack_status->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(context, {}));
  ReturnOnFailure(builder, *function, stack_status);
  auto* first = llvm::BasicBlock::Create(context, "execution.first", function);
  auto* ready = llvm::BasicBlock::Create(context, "execution.ready", function);
  auto* depth =
      builder.CreateLoad(builder.getInt64Ty(), address(NativeMemoryLayout::kExecutionDepth));
  builder.CreateCondBr(builder.CreateICmpEQ(depth, builder.getInt64(0)), first, ready);
  builder.SetInsertPoint(first);
  // 宿主重入借用当前执行时必须保留外层 catch 的异常状态。
  builder.CreateStore(llvm::Constant::getNullValue(NativeRecordType(context)),
                      address(NativeMemoryLayout::kExecutionException));
  builder.CreateStore(builder.getInt8(0),
                      address(NativeMemoryLayout::kExecutionExceptionAllocationFailed));
  builder.CreateBr(ready);
  builder.SetInsertPoint(ready);
}

void EmitWrapper(llvm::Module& module,
                 const ir::Function& source,
                 std::uint32_t function_index,
                 const GeneratedFunction& function,
                 llvm::Constant* image) {
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
  EmitExecutionBoundary(builder);
  llvm::Value* current_image = image;
  if (function.baseline_body != nullptr) {
    using runtime::NativeMemoryLayout;
    auto* execution = entry->getArg(0);
    auto* instance_address = builder.CreateConstGEP1_64(builder.getInt8Ty(), execution,
                                                        NativeMemoryLayout::kExecutionInstance);
    auto* instance = builder.CreateLoad(builder.getPtrTy(), instance_address);
    auto* data_address = builder.CreateConstGEP1_64(builder.getInt8Ty(), instance,
                                                    NativeMemoryLayout::kInstanceData);
    auto* data = builder.CreateLoad(builder.getPtrTy(), data_address);
    auto* image_address =
        builder.CreateConstGEP1_64(builder.getInt8Ty(), data, NativeMemoryLayout::kProgramImage);
    current_image = builder.CreateLoad(builder.getPtrTy(), image_address);
  }
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
    arguments.push_back(LoadImport(builder, module, entry->getArg(0), function_index));
  }
  arguments.push_back(result);
  llvm::Value* status = nullptr;
  if (function.baseline_body != nullptr) {
    auto helper = module.getOrInsertFunction("vela_native_v4_can_optimize", builder.getInt8Ty(),
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
  builder.CreateStore(EncodeValue(builder, value, source.return_type), entry->getArg(3));
  builder.CreateRet(status);
}

}  // namespace vela::compiler::native
