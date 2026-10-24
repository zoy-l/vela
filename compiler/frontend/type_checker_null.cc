#include "compiler/frontend/type_checker.h"

#include "compiler/frontend/ast.h"
#include "compiler/frontend/control_flow.h"

namespace vela::frontend {

auto TypeChecker::IntersectFacts(const NullFacts& left, const NullFacts& right) -> NullFacts {
  if (left.size() > right.size()) {
    return IntersectFacts(right, left);
  }
  NullFacts result;
  for (const auto binding_id : left) {
    if (right.contains(binding_id)) {
      result.insert(binding_id);
    }
  }
  return result;
}

void TypeChecker::RefineCondition(const Expression& condition, ConditionTruth outcome) {
  using Kind = Expression::Kind;
  using Truth = ConditionTruth;
  if (condition.kind == Kind::Unary && condition.text == "!") {
    RefineCondition(*condition.operand,
                    outcome == Truth::AlwaysTrue ? Truth::AlwaysFalse : Truth::AlwaysTrue);
    return;
  }
  if (condition.kind != Kind::Binary) {
    return;
  }
  if ((condition.text == "&&" && outcome == Truth::AlwaysTrue) ||
      (condition.text == "||" && outcome == Truth::AlwaysFalse)) {
    RefineCondition(*condition.left, outcome);
    // 右侧可能重写左侧刚刚检查的局部变量，不能恢复已失效的事实。
    InvalidateAssignments(*condition.right);
    RefineCondition(*condition.right, outcome);
    return;
  }
  if (condition.text != "==" && condition.text != "!=") {
    return;
  }
  const Expression* name = nullptr;
  if (condition.left->kind == Kind::Name && condition.right->kind == Kind::NullLiteral) {
    name = condition.left.get();
  } else if (condition.right->kind == Kind::Name && condition.left->kind == Kind::NullLiteral) {
    name = condition.right.get();
  }
  if (name == nullptr) {
    return;
  }
  const auto* binding = LookupLocal(name->text);
  if (binding == nullptr || !binding->type.nullable) {
    return;
  }
  if ((condition.text == "!=") == (outcome == Truth::AlwaysTrue)) {
    non_null_.insert(binding->id);
  } else {
    non_null_.erase(binding->id);
  }
}

void TypeChecker::InvalidateAssignments(const Expression& expression) {
  if (non_null_.empty()) {
    return;
  }
  if (expression.kind == Expression::Kind::Binary && expression.text == "=" &&
      expression.left->kind == Expression::Kind::Name) {
    const auto* binding = LookupLocal(expression.left->text);
    if (binding != nullptr) {
      non_null_.erase(binding->id);
    }
  }
  for (const auto* child : {
           expression.left.get(),
           expression.right.get(),
           expression.operand.get(),
           expression.callee.get(),
           expression.object.get(),
           expression.index.get(),
       }) {
    if (child != nullptr) {
      InvalidateAssignments(*child);
    }
  }
  for (const auto& child : expression.elements) {
    InvalidateAssignments(*child);
  }
  for (const auto& argument : expression.arguments) {
    InvalidateAssignments(*argument.value);
  }
}

void TypeChecker::InvalidateAssignments(const Statement& statement) {
  if (non_null_.empty()) {
    return;
  }
  for (const auto* expression :
       {statement.expression.get(), statement.condition.get(), statement.increment.get()}) {
    if (expression != nullptr) {
      InvalidateAssignments(*expression);
    }
  }
  for (const auto* child :
       {statement.then_branch.get(), statement.else_branch.get(), statement.initializer.get()}) {
    if (child != nullptr) {
      InvalidateAssignments(*child);
    }
  }
  for (const auto& child : statement.statements) {
    InvalidateAssignments(*child);
  }
  for (const auto& branch : statement.cases) {
    InvalidateAssignments(*branch.body);
  }
}

}  // namespace vela::frontend
