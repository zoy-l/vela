#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

auto Lowerer::LowerStringInterpolation(const frontend::Expression& expression) -> LoweredValue {
  std::vector<const frontend::Expression*> parts;
  parts.reserve(expression.elements.size());
  for (const auto& element : expression.elements) {
    if (element->kind != frontend::Expression::Kind::StringLiteral || !element->text.empty()) {
      parts.push_back(element.get());
    }
  }
  const auto base = next_register_;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    static_cast<void>(AllocateRegister(expression.location));
  }
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
  const auto result = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::StringInterpolate,
      .dst = result,
      .lhs = base,
      .rhs = static_cast<RegisterId>(parts.size()),
      .location = expression.location,
  });
  return {
      .reg = result,
      .type = {.kind = frontend::TypeDescriptor::Kind::String, .name = "string"},
  };
}

auto Lowerer::LowerBuiltin(const frontend::Expression& expression) -> LoweredValue {
  const auto builtin = model_.builtin_calls.at(&expression);
  const auto& signature = frontend::BuiltinSignature(builtin);
  const auto base = next_register_;
  const auto arity = BuiltinArity(builtin);
  for (std::size_t index = 0; index < arity; ++index) {
    static_cast<void>(AllocateRegister(expression.location));
  }
  const auto kind = expression.kind;
  const bool indexed = kind == frontend::Expression::Kind::Index;
  const bool property = kind == frontend::Expression::Kind::Member;
  const bool method = kind == frontend::Expression::Kind::Call &&
                      expression.callee->kind == frontend::Expression::Kind::Member;
  const std::size_t receiver_count = indexed || property || method ? 1 : 0;
  if (receiver_count != 0) {
    const auto& object = method ? *expression.callee->object : *expression.object;
    const auto receiver = LowerExpression(object);
    Emit({.opcode = Opcode::Move, .dst = base, .lhs = receiver.reg, .location = object.location});
  }
  if (indexed) {
    const auto index = LowerExpression(*expression.index);
    Emit({
        .opcode = Opcode::Move,
        .dst = static_cast<RegisterId>(base + 1),
        .lhs = index.reg,
        .location = expression.location,
    });
  } else {
    std::size_t positional = 0;
    for (const auto& argument : expression.arguments) {
      std::size_t index = positional++;
      if (!argument.name.empty()) {
        const auto parameter = std::ranges::find(signature.parameter_order, argument.name);
        index = static_cast<std::size_t>(parameter - signature.parameter_order.begin());
      }
      const auto value = LowerExpression(*argument.value);
      Emit({
          .opcode = Opcode::Move,
          .dst = static_cast<RegisterId>(base + receiver_count + index),
          .lhs = value.reg,
          .location = argument.location,
      });
    }
  }
  const auto result = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::BuiltinCall,
      .dst = result,
      .lhs = base,
      .index = static_cast<std::uint32_t>(builtin),
      .location = expression.location,
  });
  return {.reg = result, .type = signature.return_type};
}

}  // namespace vela::ir
