#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/type_checker.h"

#include <algorithm>

#include "compiler/frontend/flow/control_flow.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

ConditionTruth TypeChecker::EvaluateCondition(const Expression& expression) const {
  using Truth = ConditionTruth;
  if (expression.kind == Expression::Kind::BooleanLiteral) {
    return expression.boolean_value ? Truth::AlwaysTrue : Truth::AlwaysFalse;
  }
  const bool negated =
      expression.kind == Expression::Kind::Unary && expression.operation == TokenKind::Bang;
  const bool logical =
      expression.kind == Expression::Kind::Binary &&
      (expression.operation == TokenKind::AndAnd || expression.operation == TokenKind::OrOr);
  if (!negated && !logical) {
    return Truth::Unknown;
  }
  if (const auto cached = expression_flows_.find(&expression); cached != expression_flows_.end()) {
    return cached->second.condition;
  }
  if (negated) {
    return NegateConditionTruth(EvaluateCondition(*expression.operand));
  }
  const auto left = EvaluateCondition(*expression.left);
  const auto right = EvaluateCondition(*expression.right);
  if (expression.operation == TokenKind::AndAnd) {
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

bool TypeChecker::ExpressionCanComplete(const Expression& expression) const {
  if (const auto cached = expression_flows_.find(&expression); cached != expression_flows_.end()) {
    return cached->second.can_complete;
  }
  const auto found = facts_.expression_types.find(&expression);
  if (found != facts_.expression_types.end() && found->second.kind == Type::Kind::Never) {
    return false;
  }
  const auto completes = [this](const auto& child) { return ExpressionCanComplete(*child); };
  switch (expression.kind) {
    case Expression::Kind::Call:
      if (!completes(expression.callee)) {
        return false;
      }
      // 空安全调用可在不求值实参的情况下完成。
      if (expression.callee->null_safe) {
        return true;
      }
      return std::ranges::all_of(expression.arguments,
                                 [&](const auto& argument) { return completes(argument.value); });
    case Expression::Kind::Binary:
      return BinaryExpressionCanComplete(expression);
    case Expression::Kind::Conditional: {
      if (!completes(expression.operand)) {
        return false;
      }
      const auto truth = EvaluateCondition(*expression.operand);
      return (truth != ConditionTruth::AlwaysFalse && completes(expression.left)) ||
             (truth != ConditionTruth::AlwaysTrue && completes(expression.right));
    }
    case Expression::Kind::Match:
      if (!completes(expression.operand)) {
        return false;
      }
      return std::ranges::any_of(expression.arms,
                                 [&](const auto& arm) { return completes(arm.value); });
    case Expression::Kind::Unary:
    case Expression::Kind::TypeTest:
    case Expression::Kind::TypeCast:
    case Expression::Kind::Await:
      return completes(expression.operand);
    case Expression::Kind::Member:
      return completes(expression.object);
    case Expression::Kind::Index:
      return completes(expression.object) && completes(expression.index);
    case Expression::Kind::CompoundAssignment:
      return completes(expression.left) && completes(expression.right);
    case Expression::Kind::StringInterpolation:
    case Expression::Kind::ArrayLiteral:
      return std::ranges::all_of(expression.elements, completes);
    case Expression::Kind::MapLiteral:
      // 编辑器检查未完成的源码时，entry 的缺失键值在诊断后仍保留在 AST 中。
      return std::ranges::all_of(expression.entries, [&](const auto& entry) {
        return (entry.key == nullptr || completes(entry.key)) &&
               (entry.value == nullptr || completes(entry.value));
      });
    default:
      return true;
  }
}

bool TypeChecker::BinaryExpressionCanComplete(const Expression& expression) const {
  if (!ExpressionCanComplete(*expression.left)) {
    return false;
  }
  const auto operation = expression.operation;
  if (operation == TokenKind::AndAnd || operation == TokenKind::OrOr ||
      operation == TokenKind::QuestionQuestion) {
    const auto truth = EvaluateCondition(*expression.left);
    const auto requires_right =
        (operation == TokenKind::AndAnd && truth == ConditionTruth::AlwaysTrue) ||
        (operation == TokenKind::OrOr && truth == ConditionTruth::AlwaysFalse) ||
        (operation == TokenKind::QuestionQuestion &&
         expression.left->kind == Expression::Kind::NullLiteral);
    return !requires_right || ExpressionCanComplete(*expression.right);
  }
  return ExpressionCanComplete(*expression.right);
}

StatementFlow TypeChecker::MergeBranches(StatementFlow left, StatementFlow right) {
  return {
      .can_fall_through = left.can_fall_through || right.can_fall_through,
      .can_break = left.can_break || right.can_break,
      .can_continue = left.can_continue || right.can_continue,
  };
}

StatementFlow TypeChecker::CheckStatement(const Statement& statement, const Type& return_type) {
  StatementFlow flow;
  switch (statement.kind) {
    case Statement::Kind::VariableDeclaration:
      CheckVariableDeclaration(statement);
      flow.can_fall_through = ExpressionCanComplete(*statement.expression);
      break;
    case Statement::Kind::Return:
      CheckReturn(statement, return_type);
      flow.can_fall_through = false;
      break;
    case Statement::Kind::Throw: {
      auto type = CheckExpression(*statement.expression, nullptr);
      type = ParameterCapability(type, statement.expression->location);
      if (type.kind != Type::Kind::Never && !IsThrowable(type)) {
        AddDiagnostic(statement.location, "throw requires a non-null class or interface value");
      }
      flow.can_fall_through = false;
      break;
    }
    case Statement::Kind::Try:
      flow = CheckTry(statement, return_type);
      break;
    case Statement::Kind::Expression:
      CheckExpression(*statement.expression, nullptr);
      flow.can_fall_through = ExpressionCanComplete(*statement.expression);
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
    case Statement::Kind::ForEach:
      flow = CheckFor(statement, return_type);
      break;
    case Statement::Kind::Switch:
      flow = CheckSwitch(statement, return_type);
      break;
    case Statement::Kind::Break:
    case Statement::Kind::Continue:
      if (loop_depth_ == 0 && (statement.kind == Statement::Kind::Continue || switch_depth_ == 0)) {
        AddDiagnostic(statement.location, statement.kind == Statement::Kind::Break
                                              ? "break must be inside a loop or switch"
                                              : "continue must be inside a loop");
      }
      flow.can_fall_through = false;
      flow.can_break = statement.kind == Statement::Kind::Break;
      flow.can_continue = statement.kind == Statement::Kind::Continue;
      break;
  }
  facts_.control_flow.insert_or_assign(&statement, flow);
  return flow;
}

StatementFlow TypeChecker::CheckBlock(const Statement& statement, const Type& return_type) {
  BeginScope();
  const auto flow = CheckSequence(statement, return_type);
  EndScope();
  return flow;
}

StatementFlow TypeChecker::CheckScopedStatement(const Statement& statement,
                                                const Type& return_type) {
  BeginScope();
  const auto flow = CheckStatement(statement, return_type);
  EndScope();
  return flow;
}

StatementFlow TypeChecker::CheckSequence(const Statement& statement, const Type& return_type) {
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
  return flow;
}

StatementFlow TypeChecker::CheckIf(const Statement& statement, const Type& return_type) {
  const auto condition_type = CheckExpression(*statement.condition, nullptr);
  CheckBooleanOperand(*statement.condition, condition_type, "if condition must be bool");
  const auto then_branch = refinements_.BeginBranch();
  RefineCondition(*statement.condition, ConditionTruth::AlwaysTrue);
  const auto then_flow = CheckScopedStatement(*statement.then_branch, return_type);
  auto then_facts = refinements_.Capture(then_branch);
  const auto else_branch = refinements_.BeginBranch();
  RefineCondition(*statement.condition, ConditionTruth::AlwaysFalse);
  const auto else_flow = statement.else_branch
                             ? CheckScopedStatement(*statement.else_branch, return_type)
                             : StatementFlow();
  const auto else_facts = refinements_.Capture(else_branch);
  const auto condition = EvaluateCondition(*statement.condition);
  StatementFlow flow;
  if (condition == ConditionTruth::AlwaysTrue) {
    refinements_.Apply(then_facts);
    flow = then_flow;
    if (statement.else_branch) {
      AddDiagnostic(statement.else_branch->location, "unreachable statement");
    }
  } else if (condition == ConditionTruth::AlwaysFalse) {
    refinements_.Apply(else_facts);
    flow = else_flow;
    AddDiagnostic(statement.then_branch->location, "unreachable statement");
  } else {
    flow = MergeBranches(then_flow, else_flow);
    refinements_.MergeAlternatives(
        then_facts, else_facts,
        {.first = then_flow.can_fall_through, .second = else_flow.can_fall_through});
  }
  flow.condition = condition;
  if (!ExpressionCanComplete(*statement.condition)) {
    flow = {.can_fall_through = false};
  }
  return flow;
}

StatementFlow TypeChecker::CheckWhile(const Statement& statement, const Type& return_type) {
  LoopAssignments assignments(statement);
  if (!refinements_.IsEmpty()) {
    InvalidateAssignments(assignments.Whole());
  }
  const auto condition_type = CheckExpression(*statement.condition, nullptr);
  CheckBooleanOperand(*statement.condition, condition_type, "while condition must be bool");
  ++loop_depth_;
  const auto before = refinements_.BeginBranch();
  RefineCondition(*statement.condition, ConditionTruth::AlwaysTrue);
  const auto body = CheckScopedStatement(*statement.then_branch, return_type);
  --loop_depth_;
  refinements_.Discard(before);
  if (!refinements_.IsEmpty()) {
    InvalidateAssignments(assignments.Whole());
  }
  if (!body.can_break) {
    RefineCondition(*statement.condition, ConditionTruth::AlwaysFalse);
  }
  const auto condition = EvaluateCondition(*statement.condition);
  return ExpressionCanComplete(*statement.condition) ? CheckLoopExit(statement, condition, body)
                                                     : StatementFlow{.can_fall_through = false};
}

StatementFlow TypeChecker::CheckFor(const Statement& statement, const Type& return_type) {
  if (statement.kind == Statement::Kind::ForEach) {
    return CheckForEach(statement, return_type);
  }
  BeginScope();
  bool can_enter_loop = true;
  if (statement.initializer) {
    can_enter_loop = CheckStatement(*statement.initializer, return_type).can_fall_through;
  }
  LoopAssignments assignments(statement);
  if (!refinements_.IsEmpty()) {
    InvalidateAssignments(assignments.Whole());
  }
  auto condition = ConditionTruth::AlwaysTrue;
  if (statement.condition) {
    const auto condition_type = CheckExpression(*statement.condition, nullptr);
    CheckBooleanOperand(*statement.condition, condition_type, "for condition must be bool");
    can_enter_loop = can_enter_loop && ExpressionCanComplete(*statement.condition);
    condition = EvaluateCondition(*statement.condition);
  }
  const auto before = refinements_.BeginBranch();
  if (statement.condition) {
    RefineCondition(*statement.condition, ConditionTruth::AlwaysTrue);
  }
  ++loop_depth_;
  const auto body = CheckScopedStatement(*statement.then_branch, return_type);
  --loop_depth_;
  refinements_.Discard(before);
  // continue 可从分支直接到达增量表达式，不能沿用普通落点独有的非空事实。
  if (statement.increment) {
    const auto increment = refinements_.BeginBranch();
    if (statement.condition) {
      RefineCondition(*statement.condition, ConditionTruth::AlwaysTrue);
    }
    if (!refinements_.IsEmpty()) {
      InvalidateAssignments(assignments.Body());
    }
    CheckExpression(*statement.increment, nullptr);
    refinements_.Discard(increment);
  }
  if (!refinements_.IsEmpty()) {
    InvalidateAssignments(assignments.Whole());
  }
  if (statement.condition && !body.can_break) {
    RefineCondition(*statement.condition, ConditionTruth::AlwaysFalse);
  }
  EndScope();
  const auto exit = CheckLoopExit(statement, condition, body);
  return can_enter_loop ? exit : StatementFlow{.can_fall_through = false};
}

StatementFlow TypeChecker::CheckLoopExit(const Statement& statement,
                                         ConditionTruth condition,
                                         StatementFlow body) {
  if (condition == ConditionTruth::AlwaysFalse) {
    AddDiagnostic(statement.then_branch->location, "unreachable statement");
  }
  return {
      .can_fall_through = condition != ConditionTruth::AlwaysTrue || body.can_break,
      .condition = condition,
  };
}

StatementFlow TypeChecker::CheckForEach(const Statement& statement, const Type& return_type) {
  BeginScope();
  auto iterable = CheckExpression(*statement.iterable, nullptr);
  iterable = ParameterCapability(iterable, statement.iterable->location);
  LoopAssignments assignments(statement);
  if (!refinements_.IsEmpty()) {
    InvalidateAssignments(assignments.Body());
  }
  const auto before = refinements_.BeginBranch();
  Type element;
  const auto collection = NonNullable(iterable);
  if (iterable.kind == Type::Kind::Never) {
    element = iterable;
  } else if (iterable.kind == Type::Kind::Array) {
    element = array_elements_.at(collection.name);
  } else if (iterable.kind == Type::Kind::Set || iterable.kind == Type::Kind::Map) {
    element = generic_arguments_.at(collection.name).front();
  }
  if (!IsValid(element) || iterable.nullable) {
    AddDiagnostic(statement.iterable->location, "'in' requires a non-null array, set, or map");
  }
  DeclareLocal(statement.name, element, statement.location, BindingKind::Constant, &statement.name);
  ++loop_depth_;
  CheckScopedStatement(*statement.then_branch, return_type);
  --loop_depth_;
  refinements_.Discard(before);
  if (!refinements_.IsEmpty()) {
    InvalidateAssignments(assignments.Body());
  }
  EndScope();
  return {.can_fall_through = ExpressionCanComplete(*statement.iterable)};
}

}  // namespace vela::frontend
