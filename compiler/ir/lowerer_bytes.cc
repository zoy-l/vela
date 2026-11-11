#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/lowerer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "base/builtin.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

void Lowerer::EmitBuiltinCall(Builtin builtin,
                              RegisterId result,
                              std::span<const RegisterId> values,
                              frontend::SourceLocation location) {
  const auto base = AllocateRegisterWindow(values.size(), location);
  for (std::size_t index = 0; index < values.size(); ++index) {
    Emit({
        .opcode = Opcode::Move,
        .dst = static_cast<RegisterId>(base + index),
        .lhs = values[index],
        .location = location,
    });
  }
  EmitBuiltinCall(builtin, result, base, location);
}

void Lowerer::EmitBuiltinCall(Builtin builtin,
                              RegisterId result,
                              RegisterId base,
                              frontend::SourceLocation location) {
  Emit({
      .opcode = Opcode::BuiltinCall,
      .dst = result,
      .lhs = base,
      .index = static_cast<std::uint32_t>(builtin),
      .location = location,
  });
}

void Lowerer::EmitIndexRead(RegisterId result,
                            const LoweredValue& receiver,
                            RegisterId index,
                            frontend::SourceLocation location) {
  using Kind = frontend::TypeDescriptor::Kind;
  const auto kind = receiver.type.kind;
  const auto dynamic = kind == Kind::Dynamic;
  const auto byte_array = kind == Kind::ByteArray;
  if (kind == Kind::Array) {
    const auto name = frontend::NonNullableTypeName(receiver.type);
    RecordValueType(result, model_.array_elements.at(name));
  } else if (byte_array) {
    RecordValueType(result, {.kind = Kind::Int, .name = frontend::kIntTypeName});
  } else if (dynamic) {
    RecordValueType(result, {.kind = Kind::Dynamic, .name = frontend::kDynamicTypeName});
  }
  if (byte_array || dynamic) {
    const std::array values{receiver.reg, index};
    EmitBuiltinCall(dynamic ? Builtin::DynamicIndexRead : Builtin::ByteArrayGetIndex, result,
                    values, location);
  } else {
    Emit({
        .opcode = Opcode::GetIndex,
        .dst = result,
        .lhs = receiver.reg,
        .rhs = index,
        .location = location,
    });
  }
}

void Lowerer::EmitIndexWrite(const LoweredValue& receiver,
                             RegisterId index,
                             RegisterId value,
                             frontend::SourceLocation location) {
  const auto kind = receiver.type.kind;
  const auto dynamic = kind == frontend::TypeDescriptor::Kind::Dynamic;
  if (kind == frontend::TypeDescriptor::Kind::ByteArray || dynamic) {
    const auto result = AllocateRegister(location);
    const std::array values{receiver.reg, index, value};
    EmitBuiltinCall(dynamic ? Builtin::DynamicIndexWrite : Builtin::ByteArraySetIndex, result,
                    values, location);
  } else {
    Emit({
        .opcode = Opcode::SetIndex,
        .dst = receiver.reg,
        .lhs = index,
        .rhs = value,
        .location = location,
    });
  }
}

}  // namespace vela::ir
