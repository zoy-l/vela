#include "compiler/frontend/type_checker.h"

#include "compiler/frontend/ast.h"
#include "compiler/frontend/control_flow.h"

namespace vela::frontend {

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
      refinements_.Set(binding->id, target->second);
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
    refinements_.Set(binding->id, NonNullable(binding->type));
  } else {
    refinements_.Erase(binding->id);
  }
}

void TypeChecker::CollectAssignments(const Expression& expression, AssignmentEffects& effects) {
  if (expression.kind == Expression::Kind::Call) {
    effects.may_call = true;
  }
  if (expression.kind == Expression::Kind::Binary && expression.text == "=" &&
      expression.left->kind == Expression::Kind::Name) {
    effects.names.insert(expression.left->text);
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
      CollectAssignments(*child, effects);
    }
  }
  for (const auto& child : expression.elements) {
    CollectAssignments(*child, effects);
  }
  for (const auto& argument : expression.arguments) {
    CollectAssignments(*argument.value, effects);
  }
  for (const auto& arm : expression.arms) {
    CollectAssignments(*arm.value, effects);
  }
}

void TypeChecker::CollectAssignments(const Statement& statement, AssignmentEffects& effects) {
  for (const auto* expression : {
           statement.expression.get(),
           statement.condition.get(),
           statement.increment.get(),
           statement.iterable.get(),
       }) {
    if (expression != nullptr) {
      CollectAssignments(*expression, effects);
    }
  }
  for (const auto* child : {
           statement.then_branch.get(),
           statement.else_branch.get(),
           statement.initializer.get(),
           statement.finally_branch.get(),
       }) {
    if (child != nullptr) {
      CollectAssignments(*child, effects);
    }
  }
  for (const auto& child : statement.statements) {
    CollectAssignments(*child, effects);
  }
  for (const auto& branch : statement.cases) {
    CollectAssignments(*branch.body, effects);
  }
  for (const auto& clause : statement.catches) {
    CollectAssignments(*clause.body, effects);
  }
}

void TypeChecker::InvalidateAssignments(const Expression& expression) {
  if (!refinements_.IsEmpty()) {
    AssignmentEffects effects;
    CollectAssignments(expression, effects);
    InvalidateAssignments(effects);
  }
}

void TypeChecker::InvalidateAssignments(const Statement& statement) {
  if (!refinements_.IsEmpty()) {
    AssignmentEffects effects;
    CollectAssignments(statement, effects);
    InvalidateAssignments(effects);
  }
}

void TypeChecker::InvalidateAssignments(const AssignmentEffects& effects) {
  if (effects.may_call) {
    InvalidateCapturedRefinements();
  }
  for (const auto name : effects.names) {
    if (const auto* binding = LookupLocal(name)) {
      refinements_.Erase(binding->id);
    }
  }
}

void TypeChecker::InvalidateCapturedRefinements() {
  refinements_.Erase(captured_mutables_);
}

const TypeChecker::AssignmentEffects& TypeChecker::LoopAssignments::Body() {
  if (!body_) {
    body_.emplace();
    CollectAssignments(*statement_->then_branch, *body_);
  }
  return *body_;
}

const TypeChecker::AssignmentEffects& TypeChecker::LoopAssignments::Whole() {
  if (!whole_) {
    whole_ = Body();
    for (const auto* expression :
         {statement_->condition.get(), statement_->increment.get(), statement_->iterable.get()}) {
      if (expression != nullptr) {
        CollectAssignments(*expression, *whole_);
      }
    }
    if (statement_->initializer) {
      CollectAssignments(*statement_->initializer, *whole_);
    }
  }
  return *whole_;
}

}  // namespace vela::frontend
