#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <optional>
#include <vector>

#include "base/builtin.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

Lowerer::LoweredValue Lowerer::LowerStringInterpolation(const frontend::Expression& expression) {
  std::vector<const frontend::Expression*> parts;
  parts.reserve(expression.elements.size());
  for (const auto& element : expression.elements) {
    if (element->kind != frontend::Expression::Kind::StringLiteral || !element->text.empty()) {
      parts.push_back(element.get());
    }
  }
  const auto base = AllocateRegisterWindow(parts.size(), expression.location);
  for (std::size_t index = 0; index < parts.size(); ++index) {
    const auto part = LowerExpression(*parts.at(index));
    // 在求值下一段前保存当前值，避免后续赋值副作用改写已求值的引用。
    Emit({
        .opcode = Opcode::Move,
        .dst = static_cast<RegisterId>(base + index),
        .lhs = part.reg,
        .location = expression.location,
    });
  }
  const auto result = AllocateRegister(expression.location);
  Emit({
      .opcode = Opcode::StringInterpolate,
      .dst = result,
      .lhs = base,
      .rhs = static_cast<RegisterId>(parts.size()),
      .location = expression.location,
  });
  return {
      .reg = result,
      .type = {.kind = frontend::TypeDescriptor::Kind::String, .name = frontend::kStringTypeName},
  };
}

std::optional<Builtin> Lowerer::SnapshotCountBuiltin(Builtin builtin) {
  switch (builtin) {
    case Builtin::StringCodePoints:
      return Builtin::StringLength;
    case Builtin::StringGraphemes:
      return Builtin::StringGraphemeLength;
    case Builtin::CollectionKeys:
    case Builtin::CollectionValues:
      return Builtin::CollectionSize;
    default:
      return std::nullopt;
  }
}

void Lowerer::EmitSnapshotArray(Builtin builtin,
                                RegisterId base,
                                const frontend::TypeDescriptor& type,
                                frontend::SourceLocation location) {
  const auto count = AllocateRegister(location);
  EmitBuiltinCall(*SnapshotCountBuiltin(builtin), count, base, location);
  Emit({
      .opcode = Opcode::NewDynamicArray,
      .dst = base + 1,
      .lhs = count,
      .index = RegisterArrayType(type),
      .location = location,
  });
}

Lowerer::LoweredValue Lowerer::LowerSnapshotArray(const frontend::Expression& expression) {
  const auto builtin = model_.builtin_calls.at(&expression);
  const auto receiver = LowerExpression(*expression.callee->object);
  const auto base = AllocateRegisterWindow(2, expression.location);
  Emit({.opcode = Opcode::Move, .dst = base, .lhs = receiver.reg, .location = expression.location});
  const auto& type = model_.expression_types.at(&expression);
  EmitSnapshotArray(builtin, base, type, expression.location);
  const auto result = AllocateRegister(expression.location);
  EmitBuiltinCall(builtin, result, base, expression.location);
  return {.reg = result, .type = type};
}

}  // namespace vela::ir
