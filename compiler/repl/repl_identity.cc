#include "compiler/repl/repl_identity.h"

#include <string>

#include "compiler/frontend/flow/lambda_visitor.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::compiler {
namespace {

void NameLambda(frontend::Expression& expression,
                const frontend::Function* parent,
                const std::string& owner) {
  const auto location = expression.location;
  expression.function->name = (parent == nullptr ? owner : parent->name) + ".$lambda." +
                              std::to_string(location.line) + "." + std::to_string(location.column);
}

void NameStatement(frontend::Statement* statement, const std::string& owner) {
  if (statement != nullptr) {
    frontend::VisitLambdas(*statement,
                           [&](frontend::Expression& expression, const frontend::Function* parent) {
                             NameLambda(expression, parent, owner);
                           });
  }
}

void NameExpression(frontend::Expression* expression, const std::string& owner) {
  if (expression != nullptr) {
    frontend::VisitLambdas(*expression,
                           [&](frontend::Expression& lambda, const frontend::Function* parent) {
                             NameLambda(lambda, parent, owner);
                           });
  }
}

}  // namespace

void NameSessionLambdas(frontend::Program& program) {
  for (auto& function : program.functions) {
    NameStatement(function.body.get(), function.name);
  }
  for (auto& owner : program.classes) {
    for (auto& method : owner.methods) {
      NameStatement(method.body.get(), owner.name + "." + method.name);
    }
    if (owner.constructor) {
      NameStatement(owner.constructor->body.get(), owner.name + ".$constructor");
    }
    for (auto& field : owner.fields) {
      NameExpression(field.initializer.get(), owner.name + "." + field.name);
    }
  }
  for (auto& global : program.globals) {
    NameExpression(global.initializer.get(), "$global." + global.name);
  }
}

}  // namespace vela::compiler
