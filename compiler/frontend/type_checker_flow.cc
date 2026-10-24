#include "compiler/frontend/type_checker.h"

#include "compiler/frontend/ast.h"
#include "compiler/frontend/control_flow.h"

namespace vela::frontend {
namespace {

auto EvaluateCondition(const Expression& expression) -> ConditionTruth {
  using Truth = ConditionTruth;
  if (expression.kind == Expression::Kind::BooleanLiteral) {
    return expression.boolean_value ? Truth::AlwaysTrue : Truth::AlwaysFalse;
  }
  if (expression.kind == Expression::Kind::Unary && expression.text == "!") {
    const auto operand = EvaluateCondition(*expression.operand);
    if (operand == Truth::Unknown) {
      return operand;
    }
    return operand == Truth::AlwaysTrue ? Truth::AlwaysFalse : Truth::AlwaysTrue;
  }
  if (expression.kind != Expression::Kind::Binary ||
      (expression.text != "&&" && expression.text != "||")) {
    return Truth::Unknown;
  }
  const auto left = EvaluateCondition(*expression.left);
  const auto right = EvaluateCondition(*expression.right);
  if (expression.text == "&&") {
    if (left == Truth::AlwaysFalse || right == Truth::AlwaysFalse) {
      return Truth::AlwaysFalse;
    }
    return left == Truth::AlwaysTrue ? right : Truth::Unknown;
  }
  if (left == Truth::AlwaysTrue || right == Truth::AlwaysTrue) {
    return Truth::AlwaysTrue;
  }
  return left == Truth::AlwaysFalse ? right : Truth::Unknown;
}

auto MergeBranches(StatementFlow left, StatementFlow right) -> StatementFlow {
  return {
      .can_fall_through = left.can_fall_through || right.can_fall_through,
      .can_break = left.can_break || right.can_break,
      .can_continue = left.can_continue || right.can_continue,
  };
}

}  // namespace

auto TypeChecker::CheckStatement(const Statement& statement, const Type& return_type)
    -> StatementFlow {
  StatementFlow flow;
  switch (statement.kind) {
    case Statement::Kind::VariableDeclaration:
      CheckVariableDeclaration(statement);
      break;
    case Statement::Kind::Return:
      CheckReturn(statement, return_type);
      flow.can_fall_through = false;
      break;
    case Statement::Kind::Expression:
      CheckExpression(*statement.expression, nullptr);
      break;
    case Statement::Kind::Block:
      flow = CheckBlock(statement, return_type);
      break;
    case Statement::Kind::If:
      flow = CheckIf(statement, return_type);
      break;
    case Statement::Kind::While:
      flow = CheckWhile(statement, return_type);
      break;
    case Statement::Kind::For:
      flow = CheckFor(statement, return_type);
      break;
    case Statement::Kind::Break:
    case Statement::Kind::Continue:
      if (loop_depth_ == 0) {
        AddDiagnostic(statement.location, "loop control statement must be inside a loop");
      }
      flow.can_fall_through = false;
      flow.can_break = statement.kind == Statement::Kind::Break;
      flow.can_continue = statement.kind == Statement::Kind::Continue;
      break;
  }
  control_flow_.insert_or_assign(&statement, flow);
  return flow;
}

auto TypeChecker::CheckBlock(const Statement& statement, const Type& return_type) -> StatementFlow {
  BeginScope();
  StatementFlow flow;
  for (const auto& child : statement.statements) {
    const auto next = CheckStatement(*child, return_type);
    if (!flow.can_fall_through) {
      AddDiagnostic(child->location, "unreachable statement");
      continue;
    }
    flow.can_fall_through = next.can_fall_through;
    flow.can_break = flow.can_break || next.can_break;
    flow.can_continue = flow.can_continue || next.can_continue;
  }
  EndScope();
  return flow;
}

auto TypeChecker::CheckIf(const Statement& statement, const Type& return_type) -> StatementFlow {
  const auto condition_type = CheckExpression(*statement.condition, nullptr);
  if (condition_type.kind != Type::Kind::Bool) {
    AddDiagnostic(statement.condition->location, "if condition must be bool");
  }
  const auto then_flow = CheckStatement(*statement.then_branch, return_type);
  const auto else_flow =
      statement.else_branch ? CheckStatement(*statement.else_branch, return_type) : StatementFlow();
  const auto condition = EvaluateCondition(*statement.condition);
  StatementFlow flow;
  if (condition == ConditionTruth::AlwaysTrue) {
    flow = then_flow;
    if (statement.else_branch) {
      AddDiagnostic(statement.else_branch->location, "unreachable statement");
    }
  } else if (condition == ConditionTruth::AlwaysFalse) {
    flow = else_flow;
    AddDiagnostic(statement.then_branch->location, "unreachable statement");
  } else {
    flow = MergeBranches(then_flow, else_flow);
  }
  flow.condition = condition;
  return flow;
}

auto TypeChecker::CheckWhile(const Statement& statement, const Type& return_type) -> StatementFlow {
  const auto condition_type = CheckExpression(*statement.condition, nullptr);
  if (condition_type.kind != Type::Kind::Bool) {
    AddDiagnostic(statement.condition->location, "while condition must be bool");
  }
  ++loop_depth_;
  const auto body = CheckStatement(*statement.then_branch, return_type);
  --loop_depth_;
  const auto condition = EvaluateCondition(*statement.condition);
  if (condition == ConditionTruth::AlwaysFalse) {
    AddDiagnostic(statement.then_branch->location, "unreachable statement");
  }
  return {
      .can_fall_through = condition != ConditionTruth::AlwaysTrue || body.can_break,
      .condition = condition,
  };
}

auto TypeChecker::CheckFor(const Statement& statement, const Type& return_type) -> StatementFlow {
  BeginScope();
  if (statement.initializer) {
    CheckStatement(*statement.initializer, return_type);
  }
  auto condition = ConditionTruth::AlwaysTrue;
  if (statement.condition) {
    const auto condition_type = CheckExpression(*statement.condition, nullptr);
    if (condition_type.kind != Type::Kind::Bool) {
      AddDiagnostic(statement.condition->location, "for condition must be bool");
    }
    condition = EvaluateCondition(*statement.condition);
  }
  if (statement.increment) {
    CheckExpression(*statement.increment, nullptr);
  }
  ++loop_depth_;
  const auto body = CheckStatement(*statement.then_branch, return_type);
  --loop_depth_;
  EndScope();
  if (condition == ConditionTruth::AlwaysFalse) {
    AddDiagnostic(statement.then_branch->location, "unreachable statement");
  }
  return {
      .can_fall_through = condition != ConditionTruth::AlwaysTrue || body.can_break,
      .condition = condition,
  };
}

}  // namespace vela::frontend
