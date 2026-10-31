#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/lowerer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "base/builtin.h"
#include "compiler/frontend/typing/type_checker.h"
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
        .lhs = values.subspan(index).front(),
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
  if (receiver.type.kind == Kind::Array) {
    const auto name = receiver.type.nullable
                          ? receiver.type.name.substr(0, receiver.type.name.size() - 1)
                          : receiver.type.name;
    RecordValueType(result, model_.array_elements.at(name));
  } else if (receiver.type.kind == Kind::ByteArray) {
    RecordValueType(result, {.kind = Kind::Int, .name = frontend::kIntTypeName});
  } else if (receiver.type.kind == Kind::Dynamic) {
    RecordValueType(result, {.kind = Kind::Dynamic, .name = frontend::kDynamicTypeName});
  }
  if (receiver.type.kind == frontend::TypeDescriptor::Kind::ByteArray ||
      receiver.type.kind == frontend::TypeDescriptor::Kind::Dynamic) {
    const std::array values{receiver.reg, index};
    EmitBuiltinCall(receiver.type.kind == frontend::TypeDescriptor::Kind::Dynamic
                        ? Builtin::DynamicIndexRead
                        : Builtin::ByteArrayGetIndex,
                    result, values, location);
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
  if (receiver.type.kind == frontend::TypeDescriptor::Kind::ByteArray ||
      receiver.type.kind == frontend::TypeDescriptor::Kind::Dynamic) {
    const auto result = AllocateTemporary(location);
    const std::array values{receiver.reg, index, value};
    EmitBuiltinCall(receiver.type.kind == frontend::TypeDescriptor::Kind::Dynamic
                        ? Builtin::DynamicIndexWrite
                        : Builtin::ByteArraySetIndex,
                    result, values, location);
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
