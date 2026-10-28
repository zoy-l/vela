#include "compiler/aot/function_emitter.h"

#include <llvm/IR/Module.h>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/opcode.h"
#include "compiler/aot/native_values.h"
#include "compiler/ir/ir.h"
#include "runtime/heap.h"

namespace vela::compiler::aot {

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
  builder_.CreateStore(DecodeValue(builder_, record, source_.value_types.at(reg)), slots_.at(reg));
}

void FunctionEmitter::StoreRoots(std::size_t index) {
  auto store =
      module_.getOrInsertFunction("vela_native_v1_root_store", builder_.getVoidTy(),
                                  builder_.getPtrTy(), builder_.getInt64Ty(), builder_.getPtrTy());
  for (const auto reg : roots_.stores.at(index)) {
    const auto& slot = roots_.slots.at(reg);
    if (!slot) {
      std::unreachable();
    }
    auto* value = Pack(Encode(reg), 0);
    builder_.CreateCall(store, {body_->getArg(0), builder_.getInt64(*slot), value});
  }
}

void FunctionEmitter::ClearRoots(std::size_t index) {
  auto clear = module_.getOrInsertFunction("vela_native_v1_root_clear", builder_.getVoidTy(),
                                           builder_.getPtrTy(), builder_.getInt64Ty());
  for (const auto reg : roots_.clears.at(index)) {
    const auto& slot = roots_.slots.at(reg);
    if (!slot) {
      std::unreachable();
    }
    builder_.CreateCall(clear, {body_->getArg(0), builder_.getInt64(*slot)});
  }
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
      for (std::size_t argument = 0; argument < count; ++argument) {
        Pack(Encode(static_cast<ir::RegisterId>(instruction.lhs + argument)), argument + 1);
      }
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
      for (std::size_t argument = 0; argument < count; ++argument) {
        Pack(Encode(static_cast<ir::RegisterId>(instruction.lhs + argument)), argument);
      }
      auto helper =
          module_.getOrInsertFunction("vela_native_v1_call_interface", status_type, pointer, word,
                                      pointer, builder_.getInt64Ty(), pointer);
      status = builder_.CreateCall(helper,
                                   {context, index, arguments_, builder_.getInt64(count), result_});
      break;
    }
    case Opcode::GetGlobal: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_get_global", status_type, pointer,
                                                word, pointer);
      status = builder_.CreateCall(helper, {context, index, result_});
      break;
    }
    case Opcode::SetGlobal: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_set_global", builder_.getVoidTy(),
                                                pointer, word, pointer);
      builder_.CreateCall(helper, {context, index, Pack(Encode(instruction.lhs), 0)});
      return {};
    }
    case Opcode::NewOk:
    case Opcode::NewErr: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_new_expected", status_type, pointer,
                                                word, pointer, status_type, pointer);
      const auto alternative = instruction.opcode == Opcode::NewOk
                                   ? runtime::ExpectedAlternative::Ok
                                   : runtime::ExpectedAlternative::Err;
      status =
          builder_.CreateCall(helper, {
                                          context,
                                          index,
                                          Pack(Encode(instruction.lhs), 0),
                                          builder_.getInt8(static_cast<std::uint8_t>(alternative)),
                                          result_,
                                      });
      break;
    }
    case Opcode::ExpectedHasValue: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_expected_has_value", status_type,
                                                pointer, pointer);
      status = builder_.CreateCall(helper, {Reference(instruction.lhs), result_});
      break;
    }
    case Opcode::ExpectedValue: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_expected_value", status_type,
                                                pointer, status_type, pointer);
      status = builder_.CreateCall(
          helper, {
                      Reference(instruction.lhs),
                      builder_.getInt8(static_cast<std::uint8_t>(instruction.index)),
                      result_,
                  });
      break;
    }
    case Opcode::IsType:
    case Opcode::CheckedCast: {
      auto* value = Pack(Encode(instruction.lhs), 0);
      auto helper = module_.getOrInsertFunction("vela_native_v1_type_test", status_type, pointer,
                                                word, pointer);
      auto* matches = builder_.CreateCall(helper, {context, index, value});
      auto* condition = builder_.CreateICmpNE(matches, builder_.getInt8(0));
      if (instruction.opcode == Opcode::IsType) {
        Store(instruction.dst, condition, {.kind = Kind::Bool, .name = "bool"});
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
    case Opcode::GetField: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_get_field", status_type, pointer,
                                                word, pointer);
      status = builder_.CreateCall(helper, {Reference(instruction.lhs), index, result_});
      break;
    }
    case Opcode::SetField: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_set_field", status_type, pointer,
                                                pointer, word, pointer);
      auto* value = Pack(Encode(instruction.lhs), 0);
      CheckStatus(builder_.CreateCall(helper, {context, Reference(instruction.dst), index, value}));
      return {};
    }
    case Opcode::GetIndex: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_get_index", status_type, pointer,
                                                builder_.getInt64Ty(), pointer);
      status = builder_.CreateCall(helper,
                                   {Reference(instruction.lhs), Payload(instruction.rhs), result_});
      break;
    }
    case Opcode::SetIndex: {
      auto helper = module_.getOrInsertFunction("vela_native_v1_set_index", status_type, pointer,
                                                pointer, builder_.getInt64Ty(), pointer);
      auto* value = Pack(Encode(instruction.rhs), 0);
      CheckStatus(builder_.CreateCall(
          helper, {context, Reference(instruction.dst), Payload(instruction.lhs), value}));
      return {};
    }
    case Opcode::BuiltinCall: {
      const auto builtin = static_cast<Builtin>(instruction.index);
      for (std::size_t argument = 0; argument < BuiltinArity(builtin); ++argument) {
        Pack(Encode(static_cast<ir::RegisterId>(instruction.lhs + argument)), argument);
      }
      auto helper = module_.getOrInsertFunction("vela_native_v1_builtin", status_type, pointer,
                                                word, pointer, pointer);
      status = builder_.CreateCall(helper, {context, index, arguments_, result_});
      break;
    }
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
      for (std::size_t argument = 0; argument < instruction.rhs; ++argument) {
        Pack(Encode(static_cast<ir::RegisterId>(instruction.lhs + argument)), argument);
      }
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
      return std::unexpected("AOT lowering is not implemented for opcode " +
                             std::to_string(static_cast<unsigned>(instruction.opcode)));
  }
  CheckStatus(status);
  Unpack(instruction.dst);
  if (instruction.opcode == Opcode::NotEqualString) {
    Store(instruction.dst, builder_.CreateNot(Load(instruction.dst)),
          {.kind = Kind::Bool, .name = "bool"});
  }
  return {};
}

}  // namespace vela::compiler::aot
