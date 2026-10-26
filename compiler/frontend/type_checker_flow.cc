#include "compiler/frontend/type_checker.h"

#include "compiler/frontend/ast.h"
#include "compiler/frontend/control_flow.h"

namespace vela::frontend {
namespace {

ConditionTruth EvaluateCondition(const Expression& expression) {
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

StatementFlow MergeBranches(StatementFlow left, StatementFlow right) {
  return {
      .can_fall_through = left.can_fall_through || right.can_fall_through,
      .can_break = left.can_break || right.can_break,
      .can_continue = left.can_continue || right.can_continue,
  };
}

}  // namespace

StatementFlow TypeChecker::CheckStatement(const Statement& statement, const Type& return_type) {
  StatementFlow flow;
  switch (statement.kind) {
    case Statement::Kind::VariableDeclaration:
      CheckVariableDeclaration(statement);
      break;
    case Statement::Kind::Return:
      CheckReturn(statement, return_type);
      flow.can_fall_through = false;
      break;
    case Statement::Kind::Throw: {
      const auto type = CheckExpression(*statement.expression, nullptr);
      if ((type.kind != Type::Kind::Class && type.kind != Type::Kind::Interface) || type.nullable) {
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
  control_flow_.insert_or_assign(&statement, flow);
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
  if (condition_type.kind != Type::Kind::Bool || condition_type.nullable) {
    AddDiagnostic(statement.condition->location, "if condition must be bool");
  }
  const auto before = refinements_;
  RefineCondition(*statement.condition, ConditionTruth::AlwaysTrue);
  const auto then_flow = CheckScopedStatement(*statement.then_branch, return_type);
  const auto then_facts = refinements_;
  refinements_ = before;
  RefineCondition(*statement.condition, ConditionTruth::AlwaysFalse);
  const auto else_flow = statement.else_branch
                             ? CheckScopedStatement(*statement.else_branch, return_type)
                             : StatementFlow();
  const auto else_facts = refinements_;
  const auto condition = EvaluateCondition(*statement.condition);
  StatementFlow flow;
  if (condition == ConditionTruth::AlwaysTrue) {
    refinements_ = then_facts;
    flow = then_flow;
    if (statement.else_branch) {
      AddDiagnostic(statement.else_branch->location, "unreachable statement");
    }
  } else if (condition == ConditionTruth::AlwaysFalse) {
    refinements_ = else_facts;
    flow = else_flow;
    AddDiagnostic(statement.then_branch->location, "unreachable statement");
  } else {
    flow = MergeBranches(then_flow, else_flow);
    if (!then_flow.can_fall_through) {
      refinements_ = else_facts;
    } else if (!else_flow.can_fall_through) {
      refinements_ = then_facts;
    } else {
      refinements_ = IntersectFacts(then_facts, else_facts);
    }
  }
  flow.condition = condition;
  return flow;
}

StatementFlow TypeChecker::CheckWhile(const Statement& statement, const Type& return_type) {
  InvalidateAssignments(statement);
  const auto condition_type = CheckExpression(*statement.condition, nullptr);
  if (condition_type.kind != Type::Kind::Bool || condition_type.nullable) {
    AddDiagnostic(statement.condition->location, "while condition must be bool");
  }
  ++loop_depth_;
  const auto before = refinements_;
  RefineCondition(*statement.condition, ConditionTruth::AlwaysTrue);
  const auto body = CheckScopedStatement(*statement.then_branch, return_type);
  --loop_depth_;
  refinements_ = before;
  InvalidateAssignments(statement);
  if (!body.can_break) {
    RefineCondition(*statement.condition, ConditionTruth::AlwaysFalse);
  }
  const auto condition = EvaluateCondition(*statement.condition);
  if (condition == ConditionTruth::AlwaysFalse) {
    AddDiagnostic(statement.then_branch->location, "unreachable statement");
  }
  return {
      .can_fall_through = condition != ConditionTruth::AlwaysTrue || body.can_break,
      .condition = condition,
  };
}

StatementFlow TypeChecker::CheckFor(const Statement& statement, const Type& return_type) {
  if (statement.kind == Statement::Kind::ForEach) {
    return CheckForEach(statement, return_type);
  }
  BeginScope();
  if (statement.initializer) {
    CheckStatement(*statement.initializer, return_type);
  }
  InvalidateAssignments(statement);
  auto condition = ConditionTruth::AlwaysTrue;
  if (statement.condition) {
    const auto condition_type = CheckExpression(*statement.condition, nullptr);
    if (condition_type.kind != Type::Kind::Bool || condition_type.nullable) {
      AddDiagnostic(statement.condition->location, "for condition must be bool");
    }
    condition = EvaluateCondition(*statement.condition);
  }
  const auto before = refinements_;
  if (statement.condition) {
    RefineCondition(*statement.condition, ConditionTruth::AlwaysTrue);
  }
  ++loop_depth_;
  const auto body = CheckScopedStatement(*statement.then_branch, return_type);
  --loop_depth_;
  // continue 可从分支直接到达增量表达式，不能沿用普通落点独有的非空事实。
  if (statement.increment) {
    refinements_ = before;
    if (statement.condition) {
      RefineCondition(*statement.condition, ConditionTruth::AlwaysTrue);
    }
    InvalidateAssignments(*statement.then_branch);
    CheckExpression(*statement.increment, nullptr);
  }
  refinements_ = before;
  InvalidateAssignments(statement);
  if (statement.condition && !body.can_break) {
    RefineCondition(*statement.condition, ConditionTruth::AlwaysFalse);
  }
  EndScope();
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
  const auto iterable = CheckExpression(*statement.iterable, nullptr);
  Type element;
  if (iterable.kind == Type::Kind::Array) {
    element = array_elements_.at(iterable.name);
  } else if (iterable.kind == Type::Kind::Set || iterable.kind == Type::Kind::Map) {
    const auto arguments = generic_arguments_.find(iterable.name);
    if (arguments != generic_arguments_.end() && !arguments->second.empty()) {
      element = arguments->second.front();
    }
  }
  if (!IsValid(element)) {
    AddDiagnostic(statement.iterable->location, "'in' requires an array, set, or map");
  }
  if (statement.destructuring) {
    if (iterable.kind != Type::Kind::Map) {
      AddDiagnostic(statement.location, "dictionary destructuring requires a map");
    } else {
      const auto arguments = generic_arguments_.find(iterable.name);
      if (arguments == generic_arguments_.end() || arguments->second.size() != 2) {
        AddDiagnostic(statement.location, "map type arguments are invalid");
      } else {
        DeclareLocal(statement.name, arguments->second.front(), statement.location,
                     BindingKind::Constant, &statement);
        DeclareLocal(statement.second_name, arguments->second.back(), statement.location,
                     BindingKind::Constant, &statement);
      }
    }
  } else {
    DeclareLocal(statement.name, element, statement.location, BindingKind::Constant, &statement);
  }
  ++loop_depth_;
  const auto body = CheckScopedStatement(*statement.then_branch, return_type);
  --loop_depth_;
  EndScope();
  return {
      .can_fall_through = true,
      .can_break = body.can_break,
      .can_continue = body.can_continue,
      .condition = ConditionTruth::AlwaysTrue,
  };
}

}  // namespace vela::frontend
