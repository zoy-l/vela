#include "compiler/ir/lowerer.h"

#include <cstddef>
#include "base/builtin.h"
#include "base/byte_access.h"
#include "base/regexp.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

void Lowerer::RecordProgramValueTypes() {
  for (auto& function : program_.functions) {
    current_function_ = &function;
    function.value_types.resize(function.register_count);
    for (const auto& instruction : function.instructions) {
      RecordInstructionValueType(instruction);
      if (instruction.opcode == Opcode::Call) {
        RecordValueType(instruction.dst, program_.functions.at(instruction.index).return_type);
      }
      if (instruction.opcode == Opcode::Return && !function.is_async) {
        RecordValueType(instruction.dst, function.return_type);
      }
    }
  }
  current_function_ = nullptr;
}

void Lowerer::RecordInstructionValueType(const Instruction& instruction) {
  using Kind = frontend::TypeDescriptor::Kind;
  switch (instruction.opcode) {
    case Opcode::LoadInt:
    case Opcode::AddInt:
    case Opcode::SubInt:
    case Opcode::MulInt:
    case Opcode::DivInt:
    case Opcode::ModInt:
    case Opcode::NegInt:
    case Opcode::BitNotInt:
    case Opcode::BitAndInt:
    case Opcode::BitOrInt:
    case Opcode::BitXorInt:
    case Opcode::ShiftLeftInt:
    case Opcode::ShiftRightInt:
    case Opcode::ShiftUnsignedRightInt:
      RecordValueType(instruction.dst, {.kind = Kind::Int, .name = "int"});
      break;
    case Opcode::LoadDouble:
    case Opcode::AddDouble:
    case Opcode::SubDouble:
    case Opcode::MulDouble:
    case Opcode::DivDouble:
    case Opcode::NegDouble:
      RecordValueType(instruction.dst, {.kind = Kind::Double, .name = "double"});
      break;
    case Opcode::LoadBool:
    case Opcode::NotBool:
    case Opcode::AndBool:
    case Opcode::OrBool:
    case Opcode::EqualInt:
    case Opcode::NotEqualInt:
    case Opcode::LessInt:
    case Opcode::LessEqualInt:
    case Opcode::GreaterInt:
    case Opcode::GreaterEqualInt:
    case Opcode::EqualDouble:
    case Opcode::NotEqualDouble:
    case Opcode::LessDouble:
    case Opcode::LessEqualDouble:
    case Opcode::GreaterDouble:
    case Opcode::GreaterEqualDouble:
    case Opcode::EqualBool:
    case Opcode::NotEqualBool:
    case Opcode::EqualString:
    case Opcode::NotEqualString:
    case Opcode::EqualValue:
    case Opcode::NotEqualValue:
    case Opcode::IsType:
      RecordValueType(instruction.dst, {.kind = Kind::Bool, .name = "bool"});
      break;
    case Opcode::LoadString:
    case Opcode::StringConcat:
    case Opcode::StringInterpolate:
      RecordValueType(instruction.dst, {.kind = Kind::String, .name = "string"});
      break;
    case Opcode::LoadNull:
      RecordValueType(instruction.dst, {.kind = Kind::Null, .name = "null"});
      break;
    case Opcode::NewInstance:
      RecordValueType(instruction.dst,
                      {.kind = Kind::Class, .name = program_.classes.at(instruction.index).name});
      break;
    case Opcode::NewArray:
    case Opcode::NewDynamicArray:
      RecordValueType(instruction.dst, program_.array_types.at(instruction.index));
      break;
    case Opcode::NewCollection:
      RecordValueType(instruction.dst, program_.collection_types.at(instruction.index));
      break;
    case Opcode::CheckedCast:
      if (const auto& type = program_.runtime_types.at(instruction.index);
          type.kind == Kind::Enum) {
        // enum 的受查转换先产生盒装对象；序号由独立 GetField 定义。
        RecordValueType(instruction.dst, {.kind = Kind::Dynamic, .name = "dynamic"});
      } else {
        RecordValueType(instruction.dst, type);
      }
      break;
    case Opcode::Catch:
    case Opcode::ExceptionValue:
      RecordValueType(instruction.dst, {.kind = Kind::Dynamic, .name = "dynamic"});
      break;
    case Opcode::BuiltinCall: {
      const auto builtin = static_cast<Builtin>(instruction.index);
      if (builtin < Builtin::CollectionSize || builtin == Builtin::CollectionSize ||
          builtin == Builtin::ArrayCapacity || builtin == Builtin::StringToLowerCase ||
          builtin == Builtin::StringToUpperCase || IsByteBuiltin(builtin) ||
          IsRegExpBuiltin(builtin)) {
        RecordValueType(instruction.dst, frontend::BuiltinSignature(builtin).return_type);
      } else if (builtin == Builtin::ArrayMutation) {
        RecordValueType(instruction.dst, {.kind = Kind::Int, .name = "int"});
      } else if (builtin == Builtin::ArrayCheckMutation) {
        RecordValueType(instruction.dst, {.kind = Kind::Void, .name = "void"});
      } else if (builtin == Builtin::CollectionKeys || builtin == Builtin::CollectionValues) {
        // 投影填充调用窗口中的目标数组，并返回同一个数组。
        const auto type = current_function_->value_types.at(instruction.lhs + 1);
        RecordValueType(instruction.dst, type);
      } else if (builtin >= Builtin::DynamicBinary && builtin < Builtin::Count) {
        RecordValueType(instruction.dst, {.kind = Kind::Dynamic, .name = "dynamic"});
      }
      break;
    }
    case Opcode::Move: {
      const auto& types = current_function_->value_types;
      if (instruction.lhs < types.size()) {
        const auto type = types.at(instruction.lhs);
        RecordValueType(instruction.dst, type);
      }
      break;
    }
    default:
      break;
  }
}

void Lowerer::RecordValueType(RegisterId reg, const frontend::TypeDescriptor& type) {
  auto& types = current_function_->value_types;
  if (types.size() <= reg) {
    types.resize(static_cast<std::size_t>(reg) + 1);
  }
  auto& recorded = types.at(reg);
  using Kind = frontend::TypeDescriptor::Kind;
  if (recorded.kind == Kind::Invalid || recorded.kind == Kind::Null) {
    recorded = type;
  }
}

}  // namespace vela::ir
