#include "compiler/native/function_emitter.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "runtime/native/native_symbols.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ModRef.h>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_values.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {

llvm::Value* FunctionEmitter::Encode(ir::RegisterId reg) {
  return EncodeValue(builder_, Load(reg), source_.value_types.at(reg));
}

llvm::Value* FunctionEmitter::Pack(llvm::Value* encoded, std::size_t index) {
  auto* slot =
      builder_.CreateConstGEP1_64(NativeRecordType(module_.getContext()), arguments_, index);
  builder_.CreateStore(encoded, slot);
  return slot;
}

void FunctionEmitter::Unpack(ir::RegisterId reg) {
  auto* record = builder_.CreateLoad(NativeRecordType(module_.getContext()), result_);
  const auto& type = source_.value_types.at(reg);
  Store(reg, DecodeValue(builder_, record, type), type);
}

void FunctionEmitter::PackArguments(ir::RegisterId first,
                                    std::size_t count,
                                    ArgumentPrefix prefix) {
  const auto offset = static_cast<std::size_t>(prefix);
  for (std::size_t argument = 0; argument < count; ++argument) {
    Pack(Encode(static_cast<ir::RegisterId>(first + argument)), argument + offset);
  }
}

void FunctionEmitter::CompleteCall(llvm::Value* status, ir::RegisterId result) {
  CheckStatus(status);
  Unpack(result);
}

void FunctionEmitter::StoreRoots(std::size_t index) {
  for (const auto reg : roots_.stores.at(index)) {
    const auto& slot = roots_.slots.at(reg);
    if (!slot) {
      std::unreachable();
    }
    WriteHeapValue(RootAddress(*slot), reg, MemoryRegion::Roots);
  }
}

void FunctionEmitter::ClearRoots(std::size_t index) {
  if (leaf_entry_) {
    return;
  }
  for (const auto reg : roots_.clears.at(index)) {
    const auto& slot = roots_.slots.at(reg);
    if (!slot) {
      std::unreachable();
    }
    ClearRoot(*slot);
  }
}

llvm::Value* FunctionEmitter::RootAddress(std::size_t slot) {
  return ByteAddress(root_slots_, slot * sizeof(runtime::Value));
}

void FunctionEmitter::ClearRoot(std::size_t slot) {
  auto* clear = StoreMemory(builder_.getInt8(NativeKind(Kind::Null)),
                            ByteAddress(RootAddress(slot), runtime::NativeMemoryLayout::kValueKind),
                            MemoryRegion::Roots);
  clear->setMetadata(kNativeRootClearMetadata, llvm::MDNode::get(module_.getContext(), {}));
}

std::expected<void, std::string> FunctionEmitter::EmitBuiltin(const ir::Instruction& instruction) {
  const auto builtin = static_cast<Builtin>(instruction.index);
  if (builtin == Builtin::ScalarToString) {
    const auto& type = source_.value_types.at(instruction.lhs);
    if (type.nullable ||
        (type.kind != Kind::Int && type.kind != Kind::Double && type.kind != Kind::Bool)) {
      return std::unexpected("native scalar formatting requires a non-null scalar operand");
    }
    auto format =
        module_.getOrInsertFunction(runtime::kNativeScalarToStringSymbol, builder_.getPtrTy(),
                                    builder_.getPtrTy(), builder_.getPtrTy());
    auto* string =
        builder_.CreateCall(format, {body_->getArg(0), Pack(Encode(instruction.lhs), 0)});
    Check(builder_.CreateIsNotNull(string), ExecutionStatus::OutOfMemory);
    Store(instruction.dst, string, {.kind = Kind::String, .name = frontend::kStringTypeName});
    return {};
  }
  if (builtin == Builtin::ScalarTextLength) {
    auto length = module_.getOrInsertFunction(runtime::kNativeScalarTextLengthSymbol,
                                              builder_.getInt64Ty(), builder_.getPtrTy());
    Store(instruction.dst, builder_.CreateCall(length, {Pack(Encode(instruction.lhs), 0)}),
          {.kind = Kind::Int, .name = frontend::kIntTypeName});
    return {};
  }
  if (builtin == Builtin::NumericToInt || builtin == Builtin::NumericToDouble) {
    const auto& type = source_.value_types.at(instruction.lhs);
    if (type.nullable || (type.kind != Kind::Int && type.kind != Kind::Double)) {
      return std::unexpected("native numeric conversion requires a non-null numeric operand");
    }
    EmitNumericConversion(instruction, builtin);
    return {};
  }
  if (builtin == Builtin::ArrayLength || builtin == Builtin::ArrayCapacity ||
      builtin == Builtin::ArrayMutation || builtin == Builtin::ArrayCheckMutation ||
      builtin == Builtin::ArrayPush) {
    EmitArrayBuiltin(instruction, builtin);
    return {};
  }
  if (builtin == Builtin::StringLength || builtin == Builtin::StringByteLength) {
    auto* string = Reference(instruction.lhs);
    Check(builder_.CreateIsNotNull(string), ExecutionStatus::TypeError);
    const auto offset = builtin == Builtin::StringLength
                            ? runtime::NativeMemoryLayout::kStringLength
                            : runtime::NativeMemoryLayout::kObjectLength;
    auto* length = builder_.CreateLoad(builder_.getInt32Ty(), ByteAddress(string, offset));
    Store(instruction.dst, builder_.CreateZExt(length, builder_.getInt64Ty()),
          {.kind = Kind::Int, .name = frontend::kIntTypeName});
    return {};
  }
  if (builtin == Builtin::MapGet || builtin == Builtin::MapSet) {
    auto* map = Reference(instruction.lhs);
    Check(builder_.CreateIsNotNull(map), ExecutionStatus::TypeError);
    auto* key = Pack(Encode(instruction.lhs + 1), 0);
    auto* pointer = builder_.getPtrTy();
    if (builtin == Builtin::MapGet) {
      auto get = module_.getOrInsertFunction(runtime::kNativeMapGetSymbol, builder_.getVoidTy(),
                                             pointer, pointer, pointer);
      auto& function = *module_.getFunction(runtime::kNativeMapGetSymbol);
      function.setMemoryEffects(llvm::MemoryEffects::readOnly().getWithModRef(
          llvm::MemoryEffects::Location::ArgMem, llvm::ModRefInfo::ModRef));
      function.addParamAttr(1, llvm::Attribute::ReadOnly);
      function.addParamAttr(2, llvm::Attribute::WriteOnly);
      function.addParamAttr(2, llvm::Attribute::NoAlias);
      builder_.CreateCall(get, {map, key, result_});
      Unpack(instruction.dst);
    } else {
      auto set = module_.getOrInsertFunction(runtime::kNativeMapSetSymbol, builder_.getInt8Ty(),
                                             pointer, pointer, pointer, pointer);
      auto* value = Pack(Encode(instruction.lhs + 2), 1);
      CheckStatus(builder_.CreateCall(set, {body_->getArg(0), map, key, value}));
      Store(instruction.dst, builder_.getFalse(),
            {.kind = Kind::Null, .name = frontend::kNullTypeName});
    }
    return {};
  }
  if (builtin == Builtin::ArrayReserve) {
    using runtime::NativeMemoryLayout;
    auto* array = Reference(instruction.lhs);
    Check(builder_.CreateIsNotNull(array), ExecutionStatus::TypeError);
    auto* length_address = ByteAddress(array, NativeMemoryLayout::kObjectLength);
    auto* length = LoadMemory(builder_.getInt32Ty(), length_address, MemoryRegion::HeapHeader);
    auto* requested = Payload(instruction.lhs + 1);
    PackArguments(instruction.lhs, BuiltinArity(builtin));
    auto reserve = module_.getOrInsertFunction(runtime::kNativeBuiltinSymbol, builder_.getInt8Ty(),
                                               builder_.getPtrTy(), builder_.getInt32Ty(),
                                               builder_.getPtrTy(), builder_.getPtrTy());
    CompleteCall(
        builder_.CreateCall(
            reserve, {body_->getArg(0), builder_.getInt32(instruction.index), arguments_, result_}),
        instruction.dst);
    // reserve 的成功契约保留长度并满足容量；将运行时保证交给 LLVM 消除后续扩容分支。
    auto* after = LoadMemory(builder_.getInt32Ty(), length_address, MemoryRegion::HeapHeader);
    auto* capacity =
        LoadMemory(builder_.getInt32Ty(), ByteAddress(array, NativeMemoryLayout::kArrayCapacity),
                   MemoryRegion::HeapHeader);
    builder_.CreateAssumption(builder_.CreateICmpEQ(after, length));
    builder_.CreateAssumption(
        builder_.CreateICmpUGE(builder_.CreateZExt(capacity, builder_.getInt64Ty()), requested));
    return {};
  }
  PackArguments(instruction.lhs, BuiltinArity(builtin));
  auto helper = module_.getOrInsertFunction(runtime::kNativeBuiltinSymbol, builder_.getInt8Ty(),
                                            builder_.getPtrTy(), builder_.getInt32Ty(),
                                            builder_.getPtrTy(), builder_.getPtrTy());
  auto* status = builder_.CreateCall(
      helper, {body_->getArg(0), builder_.getInt32(instruction.index), arguments_, result_});
  CompleteCall(status, instruction.dst);
  return {};
}

std::expected<void, std::string> FunctionEmitter::EmitReference(
    const ir::Instruction& instruction) {
  auto* context = body_->getArg(0);
  auto* pointer = builder_.getPtrTy();
  auto* word = builder_.getInt32Ty();
  auto* status_type = builder_.getInt8Ty();
  auto* const index = builder_.getInt32(instruction.index);
  llvm::Value* status = nullptr;
  switch (instruction.opcode) {
    case Opcode::LoadString: {
      auto helper = module_.getOrInsertFunction(runtime::kNativeConstantSymbol,
                                                builder_.getVoidTy(), pointer, word, pointer);
      builder_.CreateCall(helper, {context, index, result_});
      Unpack(instruction.dst);
      return {};
    }
    case Opcode::NewInstance: {
      if (!program_.classes.at(instruction.index).callable) {
        auto helper =
            module_.getOrInsertFunction(runtime::kNativeNewObjectSymbol, pointer, pointer, word);
        auto& allocation = *module_.getFunction(runtime::kNativeNewObjectSymbol);
        // 普通实例一定是新分配的对象；可调用值的版本绑定和潜在身份复用单独处理。
        allocation.addRetAttr(llvm::Attribute::NoAlias);
        allocation.addRetAttr(llvm::Attribute::getWithDereferenceableOrNullBytes(
            module_.getContext(), sizeof(runtime::HeapObject)));
        auto* object = builder_.CreateCall(helper, {context, index});
        Check(builder_.CreateIsNotNull(object), ExecutionStatus::OutOfMemory);
        if (program_.native_mode == NativeProgramMode::Closed) {
          using runtime::NativeMemoryLayout;
          auto* count = LoadMemory(word, ByteAddress(object, NativeMemoryLayout::kObjectFieldCount),
                                   MemoryRegion::HeapHeader);
          // 新实例的实际类已由分配操作确定；封闭镜像没有后续字段迁移。
          builder_.CreateAssumption(builder_.CreateICmpEQ(
              count,
              builder_.getInt32(program_.layouts.classes.at(instruction.index).field_count)));
        }
        const auto& type = source_.value_types.at(instruction.dst);
        auto* record = EncodeValue(builder_, object, {.kind = Kind::Class, .name = {}});
        Store(instruction.dst, DecodeValue(builder_, record, type), type);
        return {};
      }
      auto helper = module_.getOrInsertFunction(runtime::kNativeNewCallableSymbol, status_type,
                                                pointer, word, pointer);
      status = builder_.CreateCall(helper, {context, index, result_});
      break;
    }
    case Opcode::NewArray:
    case Opcode::NewDynamicArray: {
      auto* length = instruction.opcode == Opcode::NewArray
                         ? builder_.getInt64(std::bit_cast<std::uint64_t>(instruction.immediate))
                         : Payload(instruction.lhs);
      Check(builder_.CreateICmpSGE(length, builder_.getInt64(0)),
            ExecutionStatus::IndexOutOfBounds);
      auto helper = module_.getOrInsertFunction(runtime::kNativeNewArraySymbol, pointer, pointer,
                                                word, builder_.getInt64Ty());
      auto& allocation = *module_.getFunction(runtime::kNativeNewArraySymbol);
      allocation.addRetAttr(llvm::Attribute::NoAlias);
      allocation.addRetAttr(llvm::Attribute::getWithDereferenceableOrNullBytes(
          module_.getContext(), sizeof(runtime::HeapObject)));
      auto* array = builder_.CreateCall(helper, {context, index, length});
      Check(builder_.CreateIsNotNull(array), ExecutionStatus::OutOfMemory);
      const auto& type = source_.value_types.at(instruction.dst);
      auto* record = EncodeValue(builder_, array, {.kind = Kind::Array, .name = {}});
      Store(instruction.dst, DecodeValue(builder_, record, type), type);
      auto* actual = LoadMemory(builder_.getInt32Ty(),
                                ByteAddress(array, runtime::NativeMemoryLayout::kObjectLength),
                                MemoryRegion::HeapHeader);
      builder_.CreateAssumption(
          builder_.CreateICmpEQ(builder_.CreateZExt(actual, builder_.getInt64Ty()), length));
      return {};
    }
    case Opcode::NewCollection: {
      auto helper = module_.getOrInsertFunction(runtime::kNativeNewCollectionSymbol, status_type,
                                                pointer, word, pointer);
      status = builder_.CreateCall(helper, {context, index, result_});
      break;
    }
    case Opcode::CallIndirect: {
      if (const auto target = CallTarget(instruction_index_)) {
        EmitCall(instruction, *target);
        return {};
      }
      const auto count = instruction.index;
      PackArguments(instruction.lhs, count, ArgumentPrefix::Implicit);
      auto* callable = Pack(Encode(instruction.rhs), static_cast<std::size_t>(count) + 1);
      auto helper =
          module_.getOrInsertFunction(runtime::kNativeCallIndirectSymbol, status_type, pointer,
                                      pointer, pointer, builder_.getInt64Ty(), pointer);
      status = builder_.CreateCall(
          helper, {context, callable, arguments_, builder_.getInt64(count), result_});
      break;
    }
    case Opcode::CallInterface: {
      const auto count = static_cast<std::size_t>(instruction.immediate);
      PackArguments(instruction.lhs, count);
      if (program_.native_mode == NativeProgramMode::Reloadable) {
        const auto selector = program_.selector_declarations.at(instruction.index);
        auto helper = module_.getOrInsertFunction(runtime::kNativeCallInterfaceV3Symbol,
                                                  status_type, pointer, builder_.getInt64Ty(),
                                                  pointer, builder_.getInt64Ty(), pointer);
        status = builder_.CreateCall(helper, {
                                                 context,
                                                 builder_.getInt64(selector.value),
                                                 arguments_,
                                                 builder_.getInt64(count),
                                                 result_,
                                             });
        break;
      }
      auto helper =
          module_.getOrInsertFunction(runtime::kNativeCallInterfaceV1Symbol, status_type, pointer,
                                      word, pointer, builder_.getInt64Ty(), pointer);
      status = builder_.CreateCall(helper,
                                   {context, index, arguments_, builder_.getInt64(count), result_});
      break;
    }
    case Opcode::GetGlobal:
    case Opcode::SetGlobal: {
      EmitGlobal(instruction);
      return {};
    }
    case Opcode::IsType:
    case Opcode::CheckedCast: {
      auto* value = Pack(Encode(instruction.lhs), 0);
      auto helper = module_.getOrInsertFunction(runtime::kNativeTypeTestSymbol, status_type,
                                                pointer, word, pointer);
      auto* matches = builder_.CreateCall(helper, {context, index, value});
      auto* condition = builder_.CreateICmpNE(matches, builder_.getInt8(0));
      if (instruction.opcode == Opcode::IsType) {
        Store(instruction.dst, condition, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
      } else {
        Check(condition, ExecutionStatus::InvalidCast);
        Store(instruction.dst, Load(instruction.lhs), source_.value_types.at(instruction.lhs));
      }
      return {};
    }
    case Opcode::Catch: {
      auto helper = module_.getOrInsertFunction(runtime::kNativeCatchSymbol, builder_.getVoidTy(),
                                                pointer, pointer);
      builder_.CreateCall(helper, {context, result_});
      Unpack(instruction.dst);
      return {};
    }
    case Opcode::ExceptionValue: {
      auto helper = module_.getOrInsertFunction(runtime::kNativeExceptionValueSymbol,
                                                builder_.getVoidTy(), pointer, pointer);
      builder_.CreateCall(helper, {Pack(Encode(instruction.lhs), 0), result_});
      Unpack(instruction.dst);
      return {};
    }
    case Opcode::Throw:
    case Opcode::Rethrow: {
      const auto symbol = instruction.opcode == Opcode::Throw ? runtime::kNativeThrowSymbol
                                                              : runtime::kNativeRethrowSymbol;
      auto helper = module_.getOrInsertFunction(symbol, status_type, pointer, pointer);
      Fail(builder_.CreateCall(helper, {context, Pack(Encode(instruction.lhs), 0)}));
      return {};
    }
    case Opcode::GetField:
    case Opcode::SetField:
      EmitField(instruction);
      return {};
    case Opcode::GetIndex:
    case Opcode::SetIndex:
      EmitIndex(instruction);
      return {};
    case Opcode::BuiltinCall:
      return EmitBuiltin(instruction);
    case Opcode::StringConcat: {
      auto* left = Pack(Encode(instruction.lhs), 0);
      auto* right = Pack(Encode(instruction.rhs), 1);
      auto helper = module_.getOrInsertFunction(runtime::kNativeConcatSymbol, status_type, pointer,
                                                pointer, pointer, pointer);
      status = builder_.CreateCall(helper, {context, left, right, result_});
      break;
    }
    case Opcode::EqualString:
    case Opcode::NotEqualString: {
      auto* left = Pack(Encode(instruction.lhs), 0);
      auto* right = Pack(Encode(instruction.rhs), 1);
      auto helper = module_.getOrInsertFunction(runtime::kNativeCompareStringsSymbol,
                                                builder_.getInt8Ty(), pointer, pointer);
      auto* compared =
          builder_.CreateICmpNE(builder_.CreateCall(helper, {left, right}), builder_.getInt8(0));
      if (instruction.opcode == Opcode::NotEqualString) {
        compared = builder_.CreateNot(compared);
      }
      Store(instruction.dst, compared, {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
      return {};
    }
    case Opcode::StringInterpolate: {
      PackArguments(instruction.lhs, instruction.rhs);
      auto helper = module_.getOrInsertFunction(runtime::kNativeInterpolateSymbol, status_type,
                                                pointer, pointer, builder_.getInt64Ty(), pointer);
      status = builder_.CreateCall(
          helper, {context, arguments_, builder_.getInt64(instruction.rhs), result_});
      break;
    }
    case Opcode::Print: {
      auto* value = Pack(Encode(instruction.lhs), 0);
      auto helper = module_.getOrInsertFunction(runtime::kNativePrintSymbol, builder_.getVoidTy(),
                                                pointer, pointer);
      builder_.CreateCall(helper, {context, value});
      return {};
    }
    default:
      return std::unexpected("native lowering is not implemented for opcode " +
                             std::to_string(static_cast<unsigned>(instruction.opcode)));
  }
  CompleteCall(status, instruction.dst);
  return {};
}

}  // namespace vela::compiler::native
