#include <absl/container/flat_hash_map.h>
#include "compiler/frontend/type_checker.h"

#include "compiler/frontend/ast.h"
#include "compiler/frontend/control_flow.h"

namespace vela::frontend {

TypeChecker::Refinements TypeChecker::IntersectFacts(const Refinements& left,
                                                     const Refinements& right) {
  if (left.size() > right.size()) {
    return IntersectFacts(right, left);
  }
  Refinements result;
  for (const auto& [binding_id, type] : left) {
    const auto found = right.find(binding_id);
    if (found != right.end() && IsSameType(type, found->second)) {
      result.emplace(binding_id, type);
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
  if (condition.kind == Kind::TypeTest && outcome == Truth::AlwaysTrue &&
      condition.operand->kind == Kind::Name) {
    const auto* binding = LookupLocal(condition.operand->text);
    const auto target = type_operands_.find(&condition);
    if (binding != nullptr && target != type_operands_.end()) {
      refinements_.insert_or_assign(binding->id, target->second);
    }
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
    refinements_.insert_or_assign(binding->id, NonNullable(binding->type));
  } else {
    refinements_.erase(binding->id);
  }
}

void TypeChecker::InvalidateAssignments(const Expression& expression) {
  if (refinements_.empty()) {
    return;
  }
  if (expression.kind == Expression::Kind::Call) {
    InvalidateCapturedRefinements();
  }
  if (expression.kind == Expression::Kind::Binary && expression.text == "=" &&
      expression.left->kind == Expression::Kind::Name) {
    const auto* binding = LookupLocal(expression.left->text);
    if (binding != nullptr) {
      refinements_.erase(binding->id);
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
  for (const auto& arm : expression.arms) {
    InvalidateAssignments(*arm.value);
  }
}

void TypeChecker::InvalidateCapturedRefinements() {
  absl::erase_if(refinements_,
                 [&](const auto& fact) -> bool { return captured_mutables_.contains(fact.first); });
}

void TypeChecker::InvalidateAssignments(const Statement& statement) {
  if (refinements_.empty()) {
    return;
  }
  for (const auto* expression :
       {statement.expression.get(), statement.condition.get(), statement.increment.get()}) {
    if (expression != nullptr) {
      InvalidateAssignments(*expression);
    }
  }
  for (const auto* child : {
           statement.then_branch.get(),
           statement.else_branch.get(),
           statement.initializer.get(),
           statement.finally_branch.get(),
       }) {
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
  for (const auto& clause : statement.catches) {
    InvalidateAssignments(*clause.body);
  }
}

}  // namespace vela::frontend
