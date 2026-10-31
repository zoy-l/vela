#include "compiler/frontend/lambda_visitor.h"

#include "compiler/frontend/ast.h"

namespace vela::frontend {
namespace {

template <class Node, class Visitor>
void WalkStatement(Node* statement, Visitor visitor, const Function* parent);

template <class Node, class Visitor>
void WalkExpression(Node* expression, Visitor visitor, const Function* parent) {
  if (expression == nullptr) {
    return;
  }
  if (expression->function) {
    visitor(*expression, parent);
    auto& function = *expression->function;
    WalkStatement(function.body.get(), visitor, &function);
  }
  for (auto* child : ExpressionOperands(*expression)) {
    WalkExpression(child, visitor, parent);
  }
  for (auto& element : expression->elements) {
    WalkExpression(element.get(), visitor, parent);
  }
  for (auto& entry : expression->entries) {
    WalkExpression(entry.key.get(), visitor, parent);
    WalkExpression(entry.value.get(), visitor, parent);
  }
  for (auto& argument : expression->arguments) {
    WalkExpression(argument.value.get(), visitor, parent);
  }
  for (auto& arm : expression->arms) {
    WalkExpression(arm.pattern.get(), visitor, parent);
    WalkExpression(arm.value.get(), visitor, parent);
  }
}

template <class Node, class Visitor>
void WalkStatement(Node* statement, Visitor visitor, const Function* parent) {
  if (statement == nullptr) {
    return;
  }
  for (auto* expression : {
           statement->expression.get(),
           statement->condition.get(),
           statement->increment.get(),
           statement->iterable.get(),
       }) {
    WalkExpression(expression, visitor, parent);
  }
  for (auto* child : {
           statement->then_branch.get(),
           statement->else_branch.get(),
           statement->initializer.get(),
           statement->finally_branch.get(),
       }) {
    WalkStatement(child, visitor, parent);
  }
  for (auto& child : statement->statements) {
    WalkStatement(child.get(), visitor, parent);
  }
  for (auto& branch : statement->cases) {
    WalkExpression(branch.label.get(), visitor, parent);
    WalkStatement(branch.body.get(), visitor, parent);
  }
  for (auto& caught : statement->catches) {
    WalkStatement(caught.body.get(), visitor, parent);
  }
}

}  // namespace

void VisitLambdas(Expression& expression, LambdaVisitor visitor) {
  WalkExpression(&expression, visitor, nullptr);
}
void VisitLambdas(Statement& statement, LambdaVisitor visitor) {
  WalkStatement(&statement, visitor, nullptr);
}
void VisitLambdas(const Expression& expression, ConstLambdaVisitor visitor) {
  WalkExpression(&expression, visitor, nullptr);
}
void VisitLambdas(const Statement& statement, ConstLambdaVisitor visitor) {
  WalkStatement(&statement, visitor, nullptr);
}

}  // namespace vela::frontend
