#include "compiler/frontend/type_checker.h"

#include "compiler/frontend/ast.h"

namespace vela::frontend {

TypeChecker::Type TypeChecker::CheckTypeOperation(const Expression& expression) {
  const auto source = CheckExpression(*expression.operand, nullptr);
  const auto target = ResolveType(expression.type, false);
  type_operands_.insert_or_assign(&expression, target);
  if (!IsValid(source) || !IsValid(target)) {
    return InvalidType();
  }
  const bool source_nominal =
      source.kind == Type::Kind::Class || source.kind == Type::Kind::Interface;
  const bool target_nominal =
      target.kind == Type::Kind::Class || target.kind == Type::Kind::Interface;
  if (source.kind != Type::Kind::Null && !(source_nominal && target_nominal) &&
      !IsSameType(NonNullable(source), NonNullable(target))) {
    AddDiagnostic(
        expression.location,
        "type tests and casts require related types; numeric conversions use toInt()/toDouble()");
    return InvalidType();
  }
  return expression.kind == Expression::Kind::TypeTest
             ? Type{.kind = Type::Kind::Bool, .name = "bool"}
             : target;
}

}  // namespace vela::frontend
