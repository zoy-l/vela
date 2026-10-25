#include "compiler/frontend/type_checker.h"

#include <optional>
#include <string>
#include <utility>

#include "compiler/frontend/ast.h"

namespace vela::frontend {

auto TypeChecker::IsMatchComplete(const Type& selector, const MatchCoverage& coverage) -> bool {
  if (selector.kind == Type::Kind::Null) {
    return coverage.null;
  }
  return coverage.values && (coverage.null || !selector.nullable);
}

void TypeChecker::CheckMatchConstant(const MatchArm& arm,
                                     const Type& base,
                                     MatchCoverage& coverage) {
  const auto actual = CheckExpression(*arm.pattern, &base);
  if (IsValid(actual) && !IsSameType(base, actual)) {
    AddDiagnostic(arm.location, "match pattern type must equal the selector type");
  }
  const auto constant = EvaluateConstant(*arm.pattern);
  if (!constant) {
    AddDiagnostic(arm.location, "match pattern must be a compile-time constant");
    return;
  }
  if (!coverage.labels.insert(ConstantKey(*constant)).second) {
    AddDiagnostic(arm.location, "duplicate match pattern");
  }
  case_values_.insert_or_assign(arm.pattern.get(), *constant);
  if (base.kind == Type::Kind::Bool) {
    coverage.values = coverage.labels.size() == 2;
  } else if (base.kind == Type::Kind::Enum) {
    coverage.values = coverage.labels.size() == enum_sizes_.at(base.name);
  }
}

auto TypeChecker::CheckMatchPattern(const MatchArm& arm,
                                    const Type& selector,
                                    MatchCoverage& coverage) -> Type {
  auto base = NonNullable(selector);
  if (arm.kind == MatchArm::Kind::Constant) {
    if (arm.pattern->kind != Expression::Kind::NullLiteral) {
      CheckMatchConstant(arm, base, coverage);
      return base;
    }
    if (coverage.null) {
      AddDiagnostic(arm.location, "duplicate null pattern");
    }
    if (!selector.nullable && selector.kind != Type::Kind::Null) {
      AddDiagnostic(arm.location, "null pattern requires a nullable selector");
    }
    coverage.null = true;
    return {.kind = Type::Kind::Null, .name = "null"};
  }
  Type narrowed = selector;
  if (coverage.null) {
    narrowed = base;
  } else if (coverage.values && selector.nullable) {
    narrowed = {.kind = Type::Kind::Null, .name = "null"};
  }
  if (arm.kind == MatchArm::Kind::Binding &&
      DeclareLocal(arm.binding, narrowed, arm.location, BindingKind::Mutable, &arm)) {
    bindings_.emplace(&arm, LookupLocal(arm.binding)->id);
    match_bindings_.emplace(&arm, narrowed);
  }
  coverage.null = true;
  coverage.values = true;
  return narrowed;
}

void TypeChecker::CheckMatchResult(const Expression& expression,
                                   const Type* expected,
                                   Type& result) {
  const auto arm_type = CheckExpression(expression, IsValid(result) ? &result : nullptr);
  if (!IsValid(result)) {
    result = arm_type;
  } else if (IsValid(arm_type) && !CanAssign(result, arm_type)) {
    if (expected == nullptr && CanAssign(arm_type, result)) {
      result = arm_type;
    } else {
      AddDiagnostic(expression.location, "match arms must produce the same type");
    }
  }
}

auto TypeChecker::CheckMatch(const Expression& expression, const Type* expected) -> Type {
  const auto selector = CheckExpression(*expression.operand, nullptr);
  const auto before = refinements_;
  std::optional<Refinements> exits;
  MatchCoverage coverage;
  Type result = expected == nullptr ? InvalidType() : *expected;
  for (const auto& arm : expression.arms) {
    if (IsMatchComplete(selector, coverage)) {
      AddDiagnostic(arm.location, "unreachable match arm");
    }
    refinements_ = before;
    BeginScope();
    const auto narrowed = CheckMatchPattern(arm, selector, coverage);
    if (expression.operand->kind == Expression::Kind::Name) {
      if (const auto* binding = LookupLocal(expression.operand->text)) {
        refinements_.insert_or_assign(binding->id, narrowed);
      }
    }
    CheckMatchResult(*arm.value, expected, result);
    EndScope();
    exits = exits ? IntersectFacts(*exits, refinements_) : refinements_;
  }
  if (expression.arms.empty() || !IsMatchComplete(selector, coverage)) {
    AddDiagnostic(expression.location,
                  "match is not exhaustive; cover every value or add a wildcard/binding arm");
  }
  if (exits) {
    refinements_ = std::move(*exits);
  } else {
    refinements_ = before;
  }
  return result;
}

}  // namespace vela::frontend
