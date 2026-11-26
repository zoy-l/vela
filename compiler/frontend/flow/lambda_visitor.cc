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
    VisitLambdas(*expression->function, visitor);
  }
  VisitExpressionChildren(*expression,
                          [&](const Expression* child) { WalkExpression(child, visitor); });
}

void WalkStatement(const Statement* statement, LambdaVisitor visitor) {
  if (statement == nullptr) {
    return;
  }
  VisitStatementChildren(
      *statement, [&](const Expression* child) { WalkExpression(child, visitor); },
      [&](const Statement* child) { WalkStatement(child, visitor); });
}

}  // namespace

void VisitLambdas(const Expression& expression, LambdaVisitor visitor) {
  WalkExpression(&expression, visitor);
}
void VisitLambdas(const Statement& statement, LambdaVisitor visitor) {
  WalkStatement(&statement, visitor);
}
void VisitLambdas(const Function& function, LambdaVisitor visitor) {
  VisitFunctionChildren(
      function, [&](const Expression* child) { WalkExpression(child, visitor); },
      [&](const Statement* child) { WalkStatement(child, visitor); });
}

}  // namespace vela::frontend
