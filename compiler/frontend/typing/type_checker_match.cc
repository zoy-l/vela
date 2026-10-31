#include "compiler/frontend/typing/type_checker.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

bool TypeChecker::IsMatchComplete(const Type& selector, const MatchCoverage& coverage) {
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

TypeChecker::Type TypeChecker::CheckMatchPattern(const MatchArm& arm,
                                                 const Type& selector,
                                                 MatchCoverage& coverage) {
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
    return {.kind = Type::Kind::Null, .name = kNullTypeName};
  }
  Type narrowed = selector;
  if (coverage.null) {
    narrowed = base;
  } else if (coverage.values && selector.nullable) {
    narrowed = {.kind = Type::Kind::Null, .name = kNullTypeName};
  }
  if (arm.kind == MatchArm::Kind::Binding &&
      DeclareLocal(arm.binding, narrowed, arm.location, BindingKind::Mutable, &arm)) {
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
  const bool unresolved_return = result.kind == Type::Kind::Function &&
                                 !IsValid(function_types_.at(NonNullable(result).name).return_type);
  if (!IsValid(result) || unresolved_return) {
    result = arm_type;
  } else if (IsValid(arm_type) && !CanAssign(result, arm_type)) {
    if (expected == nullptr && CanAssign(arm_type, result)) {
      result = arm_type;
    } else {
      AddDiagnostic(expression.location, "match arms must produce the same type");
    }
  }
}

TypeChecker::Type TypeChecker::EnterMatchArm(const Expression& selector_expression,
                                             const MatchArm& arm,
                                             const Type& selector,
                                             MatchCoverage& coverage,
                                             PreparedMatchArm* prepared) {
  Type narrowed;
  if (prepared != nullptr) {
    narrowed = prepared->narrowed;
    scopes_.back() = std::move(prepared->scope);
  } else {
    if (IsMatchComplete(selector, coverage)) {
      AddDiagnostic(arm.location, "unreachable match arm");
    }
    narrowed = CheckMatchPattern(arm, selector, coverage);
  }
  if (selector_expression.kind == Expression::Kind::Name) {
    if (const auto* binding = LookupLocal(selector_expression.text)) {
      refinements_.Set(binding->id, narrowed);
    }
  }
  return narrowed;
}

TypeChecker::Type TypeChecker::CheckMatch(const Expression& expression,
                                          const Type* expected,
                                          MatchPhase phase) {
  std::vector<PreparedMatchArm> prepared;
  const auto found = prepared_matches_.find(&expression);
  const bool resumed = found != prepared_matches_.end();
  if (resumed) {
    prepared = std::move(found->second);
    prepared_matches_.erase(found);
  }
  const bool preparing = phase == MatchPhase::Prepare;
  const auto selector = preparing ? PrepareInferenceExpression(*expression.operand)
                                  : CheckExpression(*expression.operand, nullptr);
  std::optional<RefinementDelta> exits;
  MatchCoverage coverage;
  Type result = expected == nullptr ? InvalidType() : *expected;
  for (std::size_t index = 0; index < expression.arms.size(); ++index) {
    const auto& arm = expression.arms.at(index);
    const auto branch = refinements_.BeginBranch();
    BeginScope();
    const auto narrowed = EnterMatchArm(*expression.operand, arm, selector, coverage,
                                        resumed ? &prepared.at(index) : nullptr);
    if (preparing) {
      PrepareInferenceReceiver(*arm.value);
      prepared.push_back({.narrowed = narrowed, .scope = scopes_.back()});
    } else {
      CheckMatchResult(*arm.value, expected, result);
    }
    EndScope();
    auto changes = refinements_.Capture(branch);
    if (exits) {
      refinements_.Intersect(*exits, changes);
    } else {
      exits = std::move(changes);
    }
  }
  if (!resumed && (expression.arms.empty() || !IsMatchComplete(selector, coverage))) {
    AddDiagnostic(expression.location,
                  "match is not exhaustive; cover every value or add a wildcard/binding arm");
  }
  if (exits) {
    refinements_.Apply(*exits);
  }
  if (preparing) {
    prepared_matches_.emplace(&expression, std::move(prepared));
  }
  return result;
}

}  // namespace vela::frontend
