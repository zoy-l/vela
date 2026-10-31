#include "compiler/native/function_emitter.h"
#include "compiler/native/integer_loop_optimizer.h"

#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
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
#include "runtime/native_memory_layout.h"
#include "runtime/value.h"

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
    WriteHeapValue(RootAddress(*slot), reg);
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
  auto* clear =
      builder_.CreateStore(builder_.getInt8(NativeKind(Kind::Null)),
                           ByteAddress(RootAddress(slot), runtime::NativeMemoryLayout::kValueKind));
  clear->setMetadata(kNativeRootClearMetadata, llvm::MDNode::get(module_.getContext(), {}));
}

std::expected<void, std::string> FunctionEmitter::EmitBuiltin(const ir::Instruction& instruction) {
  const auto builtin = static_cast<Builtin>(instruction.index);
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
  PackArguments(instruction.lhs, BuiltinArity(builtin));
  auto helper = module_.getOrInsertFunction("vela_native_v1_builtin", builder_.getInt8Ty(),
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
      auto helper = module_.getOrInsertFunction("vela_native_v1_constant", builder_.getVoidTy(),
                                                pointer, word, pointer);
      builder_.CreateCall(helper, {context, index, result_});
      Unpack(instruction.dst);
      return {};
    }
    case Opcode::NewInstance: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_new_instance", status_type, pointer,
                                                word, pointer);
      status = builder_.CreateCall(helper, {context, index, result_});
      break;
    }
    case Opcode::NewArray:
    case Opcode::NewDynamicArray: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_new_array", status_type, pointer,
                                                word, pointer, builder_.getInt64Ty());
      auto* length = instruction.opcode == Opcode::NewArray
                         ? builder_.getInt64(std::bit_cast<std::uint64_t>(instruction.immediate))
                         : Payload(instruction.lhs);
      status = builder_.CreateCall(helper, {context, index, result_, length});
      break;
    }
    case Opcode::NewCollection: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_new_collection", status_type,
                                                pointer, word, pointer);
      status = builder_.CreateCall(helper, {context, index, result_});
      break;
    }
    case Opcode::CallIndirect: {
      const auto count = instruction.index;
      PackArguments(instruction.lhs, count, ArgumentPrefix::Implicit);
      auto* callable = Pack(Encode(instruction.rhs), static_cast<std::size_t>(count) + 1);
      auto helper =
          module_.getOrInsertFunction("vela_native_v1_call_indirect", status_type, pointer, pointer,
                                      pointer, builder_.getInt64Ty(), pointer);
      status = builder_.CreateCall(
          helper, {context, callable, arguments_, builder_.getInt64(count), result_});
      break;
    }
    case Opcode::CallInterface: {
      const auto count = static_cast<std::size_t>(instruction.immediate);
      PackArguments(instruction.lhs, count);
      if (program_.native_mode == NativeProgramMode::Reloadable) {
        const auto selector = program_.selector_declarations.at(instruction.index);
        auto helper = module_.getOrInsertFunction("vela_native_v3_call_interface", status_type,
                                                  pointer, builder_.getInt64Ty(), pointer,
                                                  builder_.getInt64Ty(), pointer);
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
          module_.getOrInsertFunction("vela_native_v1_call_interface", status_type, pointer, word,
                                      pointer, builder_.getInt64Ty(), pointer);
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
      auto helper = module_.getOrInsertFunction("vela_native_v1_type_test", status_type, pointer,
                                                word, pointer);
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
      auto helper = module_.getOrInsertFunction("vela_native_v1_catch", builder_.getVoidTy(),
                                                pointer, pointer);
      builder_.CreateCall(helper, {context, result_});
      Unpack(instruction.dst);
      return {};
    }
    case Opcode::ExceptionValue: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_exception_value",
                                                builder_.getVoidTy(), pointer, pointer);
      builder_.CreateCall(helper, {Pack(Encode(instruction.lhs), 0), result_});
      Unpack(instruction.dst);
      return {};
    }
    case Opcode::Throw:
    case Opcode::Rethrow: {
      const auto* symbol =
          instruction.opcode == Opcode::Throw ? "vela_native_v1_throw" : "vela_native_v1_rethrow";
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
    case Opcode::StringConcat:
    case Opcode::EqualString:
    case Opcode::NotEqualString: {
      auto* left = Pack(Encode(instruction.lhs), 0);
      auto* right = Pack(Encode(instruction.rhs), 1);
      if (instruction.opcode == Opcode::StringConcat) {
        auto helper = module_.getOrInsertFunction("vela_native_v1_concat", status_type, pointer,
                                                  pointer, pointer, pointer);
        status = builder_.CreateCall(helper, {context, left, right, result_});
      } else {
        auto helper = module_.getOrInsertFunction("vela_native_v1_compare_strings", status_type,
                                                  pointer, pointer, pointer);
        status = builder_.CreateCall(helper, {left, right, result_});
      }
      break;
    }
    case Opcode::StringInterpolate: {
      PackArguments(instruction.lhs, instruction.rhs);
      auto helper = module_.getOrInsertFunction("vela_native_v1_interpolate", status_type, pointer,
                                                pointer, builder_.getInt64Ty(), pointer);
      status = builder_.CreateCall(
          helper, {context, arguments_, builder_.getInt64(instruction.rhs), result_});
      break;
    }
    case Opcode::Print: {
      auto* value = Pack(Encode(instruction.lhs), 0);
      auto helper = module_.getOrInsertFunction("vela_native_v1_print", builder_.getVoidTy(),
                                                pointer, pointer);
      builder_.CreateCall(helper, {context, value});
      return {};
    }
    default:
      return std::unexpected("native lowering is not implemented for opcode " +
                             std::to_string(static_cast<unsigned>(instruction.opcode)));
  }
  CompleteCall(status, instruction.dst);
  if (instruction.opcode == Opcode::NotEqualString) {
    Store(instruction.dst, builder_.CreateNot(Load(instruction.dst)),
          {.kind = Kind::Bool, .name = frontend::kBoolTypeName});
  }
  return {};
}

}  // namespace vela::compiler::native
