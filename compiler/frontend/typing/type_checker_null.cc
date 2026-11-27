#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/frontend/typing/type_names.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "compiler/frontend/flow/control_flow.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

void TypeChecker::RefineAssignedLocal(const LocalBinding& binding, const Type& value) {
  if (predicate_scope_ && predicate_scope_->binding == binding.id) {
    predicate_scope_->assigned = true;
  }
  refinements_.Invalidate(binding.id);
  if (value.kind != Type::Kind::Never && value.kind != Type::Kind::Null &&
      IsSubtype(value, binding.type) &&
      !IsSameType(value, binding.type)) {
    refinements_.Set(binding.id, value);
    return;
  }
  if (binding.type.kind != Type::Kind::Dynamic && IsPotentiallyNullable(binding.type) &&
      IsValid(value) && !IsPotentiallyNullable(value)) {
    refinements_.Set(binding.id, NonNullable(binding.type));
  }
}

void TypeChecker::DiagnoseOperandMismatch(const Expression& operand,
                                          const Type& type,
                                          std::string_view message) {
  if (type.nullable || type.kind == Type::Kind::Null) {
    const auto subject = operand.kind == Expression::Kind::Name
                             ? "'" + operand.text + "'"
                             : "operand of type '" + TypeNameFor(type) + "'";
    AddDiagnostic(operand.location, subject + " may be null; check for null before this operation");
    return;
  }
  AddDiagnostic(operand.location, std::string(message));
}

void TypeChecker::DiagnoseBinaryOperandMismatch(const Expression& expression,
                                                const Type& left,
                                                const Type& right,
                                                std::string_view message) {
  const bool nullable_left = left.nullable || left.kind == Type::Kind::Null;
  const bool nullable_right = right.nullable || right.kind == Type::Kind::Null;
  if (nullable_left) {
    DiagnoseOperandMismatch(*expression.left, left, message);
  }
  if (nullable_right) {
    DiagnoseOperandMismatch(*expression.right, right, message);
  }
  if (!nullable_left && !nullable_right) {
    AddDiagnostic(expression.location, std::string(message));
  }
}

void TypeChecker::RefineCondition(const Expression& condition, ConditionTruth outcome) {
  using Kind = Expression::Kind;
  using Truth = ConditionTruth;
  if (condition.kind == Kind::Name) {
    const auto* binding = LookupLocal(condition.text);
    const auto alias = binding == nullptr ? nullptr : refinements_.FindCondition(binding->id);
    if (alias) {
      const auto& types = outcome == Truth::AlwaysTrue ? alias->when_true : alias->when_false;
      refinements_.Apply({.types = types, .conditions = {}});
    }
    return;
  }
  if (condition.kind == Kind::Call) {
    RefinePredicateCall(condition, outcome);
    return;
  }
  if (condition.kind == Kind::Unary && condition.operation == TokenKind::Bang) {
    RefineCondition(*condition.operand, NegateConditionTruth(outcome));
    return;
  }
  if (condition.kind == Kind::TypeTest && condition.operand->kind == Kind::Name) {
    RefineTypeTest(condition, outcome);
    return;
  }
  if (condition.kind == Kind::Conditional) {
    RefineAlternatives(condition, outcome);
    return;
  }
  if (condition.kind != Kind::Binary) {
    return;
  }
  if ((condition.operation == TokenKind::OrOr && outcome == Truth::AlwaysTrue) ||
      (condition.operation == TokenKind::AndAnd && outcome == Truth::AlwaysFalse)) {
    RefineAlternatives(condition, outcome);
    return;
  }
  if ((condition.operation == TokenKind::AndAnd && outcome == Truth::AlwaysTrue) ||
      (condition.operation == TokenKind::OrOr && outcome == Truth::AlwaysFalse)) {
    RefineCondition(*condition.left, outcome);
    // 右侧可能重写左侧刚刚检查的局部变量，不能恢复已失效的事实。
    InvalidateAssignments(*condition.right);
    RefineCondition(*condition.right, outcome);
    return;
  }
  RefineNullComparison(condition, outcome);
}

TypeChecker::ConditionFacts TypeChecker::CaptureCondition(const Expression& expression) {
  const auto capture = [&](ConditionTruth outcome) {
    const auto checkpoint = refinements_.BeginBranch();
    RefineCondition(expression, outcome);
    auto changes = refinements_.Capture(checkpoint);
    absl::erase_if(changes.types, [](const auto& entry) { return !entry.second; });
    return std::move(changes.types);
  };
  auto when_true = capture(ConditionTruth::AlwaysTrue);
  auto when_false = capture(ConditionTruth::AlwaysFalse);
  if (when_true.empty() && when_false.empty()) {
    return nullptr;
  }
  return std::make_shared<ConditionRefinements>(std::move(when_true), std::move(when_false));
}

void TypeChecker::RefineNullComparison(const Expression& condition, ConditionTruth outcome) {
  using Kind = Expression::Kind;
  using Truth = ConditionTruth;
  if (condition.operation != TokenKind::EqualEqual && condition.operation != TokenKind::BangEqual) {
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
  if (binding == nullptr || !IsPotentiallyNullable(binding->type)) {
    return;
  }
  if ((condition.operation == TokenKind::BangEqual) == (outcome == Truth::AlwaysTrue)) {
    refinements_.Set(binding->id, NonNullable(binding->type));
  } else {
    refinements_.Set(binding->id, {.kind = Type::Kind::Null, .name = kNullTypeName});
  }
}

void TypeChecker::RefineTypeTest(const Expression& condition, ConditionTruth outcome) {
  const auto* binding = LookupLocal(condition.operand->text);
  const auto target = facts_.type_operands.find(&condition);
  if (binding == nullptr || target == facts_.type_operands.end()) {
    return;
  }
  if (outcome == ConditionTruth::AlwaysFalse) {
    if (target->second.kind == Type::Kind::Null) {
      refinements_.Set(binding->id, NonNullable(binding->type));
    } else if (binding->type.nullable && !target->second.nullable &&
               IsSubtype(NonNullable(binding->type), target->second)) {
      refinements_.Set(binding->id, {.kind = Type::Kind::Null, .name = kNullTypeName});
    }
    return;
  }
  const auto type =
      binding->type.kind == Type::Kind::Parameter && IsSubtype(binding->type, target->second)
          ? NonNullable(binding->type)
          : target->second;
  refinements_.Set(binding->id, type);
}

void TypeChecker::CollectAssignments(const Expression& expression, AssignmentEffects& effects) {
  if (expression.kind == Expression::Kind::Call || expression.kind == Expression::Kind::Await) {
    effects.may_call = true;
  }
  if (expression.kind == Expression::Kind::Binary && expression.operation == TokenKind::Equal &&
      expression.left->kind == Expression::Kind::Name) {
    effects.names.insert(expression.left->text);
  }
  for (const auto* child : ExpressionOperands(expression)) {
    if (child != nullptr) {
      CollectAssignments(*child, effects);
    }
  }
  for (const auto& child : expression.elements) {
    CollectAssignments(*child, effects);
  }
  for (const auto& entry : expression.entries) {
    if (entry.key) {
      CollectAssignments(*entry.key, effects);
    }
    if (entry.value) {
      CollectAssignments(*entry.value, effects);
    }
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
      refinements_.Invalidate(binding->id);
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
