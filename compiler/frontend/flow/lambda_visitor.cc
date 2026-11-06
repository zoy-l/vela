#include "compiler/frontend/flow/lambda_visitor.h"

#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {
namespace {

void WalkStatement(const Statement* statement, LambdaVisitor visitor);

void WalkExpression(const Expression* expression, LambdaVisitor visitor) {
  if (expression == nullptr) {
    return;
  }
  if (expression->function) {
    visitor(*expression);
    WalkStatement(expression->function->body.get(), visitor);
  }
  for (auto* child : ExpressionOperands(*expression)) {
    WalkExpression(child, visitor);
  }
  for (const auto& element : expression->elements) {
    WalkExpression(element.get(), visitor);
  }
  for (const auto& entry : expression->entries) {
    WalkExpression(entry.key.get(), visitor);
    WalkExpression(entry.value.get(), visitor);
  }
  for (const auto& argument : expression->arguments) {
    WalkExpression(argument.value.get(), visitor);
  }
  for (const auto& arm : expression->arms) {
    WalkExpression(arm.pattern.get(), visitor);
    WalkExpression(arm.value.get(), visitor);
  }
}

void WalkStatement(const Statement* statement, LambdaVisitor visitor) {
  if (statement == nullptr) {
    return;
  }
  for (auto* expression : {
           statement->expression.get(),
           statement->condition.get(),
           statement->increment.get(),
           statement->iterable.get(),
       }) {
    WalkExpression(expression, visitor);
  }
  for (auto* child : {
           statement->then_branch.get(),
           statement->else_branch.get(),
           statement->initializer.get(),
           statement->finally_branch.get(),
       }) {
    WalkStatement(child, visitor);
  }
  for (const auto& child : statement->statements) {
    WalkStatement(child.get(), visitor);
  }
  for (const auto& branch : statement->cases) {
    WalkExpression(branch.label.get(), visitor);
    WalkStatement(branch.body.get(), visitor);
  }
  for (const auto& caught : statement->catches) {
    WalkStatement(caught.body.get(), visitor);
  }
}

}  // namespace

void VisitLambdas(const Expression& expression, LambdaVisitor visitor) {
  WalkExpression(&expression, visitor);
}
void VisitLambdas(const Statement& statement, LambdaVisitor visitor) {
  WalkStatement(&statement, visitor);
}

}  // namespace vela::frontend
