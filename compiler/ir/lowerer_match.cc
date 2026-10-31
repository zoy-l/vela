#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

Lowerer::LoweredValue Lowerer::LowerMatch(const frontend::Expression& expression) {
  const auto original = LowerExpression(*expression.operand);
  const auto selector = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::Move,
      .dst = selector,
      .lhs = original.reg,
      .location = expression.location,
  });
  const auto result = AllocateTemporary(expression.location);
  std::vector<std::size_t> exits;
  for (const auto& arm : expression.arms) {
    std::optional<std::size_t> mismatch;
    if (arm.kind == frontend::MatchArm::Kind::Constant) {
      RegisterId label = 0;
      if (arm.pattern->kind == frontend::Expression::Kind::NullLiteral) {
        label = AllocateTemporary(arm.location);
        Emit({.opcode = Opcode::LoadNull, .dst = label, .location = arm.location});
      } else {
        label = LowerConstantLabel(*arm.pattern);
      }
      auto opcode = Opcode::EqualValue;
      using Kind = frontend::TypeDescriptor::Kind;
      if (!original.type.nullable && original.type.kind != Kind::Null) {
        if (original.type.kind == Kind::Int || original.type.kind == Kind::Enum) {
          opcode = Opcode::EqualInt;
        } else if (original.type.kind == Kind::Bool) {
          opcode = Opcode::EqualBool;
        } else if (original.type.kind == Kind::String) {
          opcode = Opcode::EqualString;
        }
      }
      const auto matches = AllocateTemporary(arm.location);
      Emit({
          .opcode = opcode,
          .dst = matches,
          .lhs = selector,
          .rhs = label,
          .location = arm.location,
      });
      mismatch = current_function_->instructions.size();
      Emit({.opcode = Opcode::JumpIfFalse, .lhs = matches, .location = arm.location});
    }
    PushScope();
    if (arm.kind == frontend::MatchArm::Kind::Binding) {
      const auto bound = AllocateTemporary(arm.location);
      Emit({.opcode = Opcode::Move, .dst = bound, .lhs = selector, .location = arm.location});
      DeclareLocal(arm.binding, bound, model_.match_bindings.at(&arm), arm.location, &arm);
    }
    const auto value = LowerExpression(*arm.value);
    Emit({.opcode = Opcode::Move, .dst = result, .lhs = value.reg, .location = arm.location});
    PopScope();
    exits.push_back(current_function_->instructions.size());
    Emit({.opcode = Opcode::Jump, .location = arm.location});
    if (mismatch) {
      current_function_->instructions.at(*mismatch).index =
          static_cast<std::uint32_t>(current_function_->instructions.size());
    }
  }
  const auto end = static_cast<std::uint32_t>(current_function_->instructions.size());
  for (const auto exit : exits) {
    current_function_->instructions.at(exit).index = end;
  }
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

}  // namespace vela::ir
