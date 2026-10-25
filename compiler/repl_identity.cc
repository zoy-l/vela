#include "compiler/repl_identity.h"

#include <string>

#include "compiler/frontend/ast.h"

namespace vela::compiler {
namespace {

void NameStatement(frontend::Statement* statement, const std::string& owner);

void NameExpression(frontend::Expression* expression, const std::string& owner) {
  if (expression == nullptr) {
    return;
  }
  if (expression->function) {
    auto& function = *expression->function;
    const auto location = expression->location;
    function.name =
        owner + ".$lambda." + std::to_string(location.line) + "." + std::to_string(location.column);
    NameStatement(function.body.get(), function.name);
  }
  for (auto* child : {
           expression->left.get(),
           expression->right.get(),
           expression->operand.get(),
           expression->callee.get(),
           expression->object.get(),
           expression->index.get(),
       }) {
    NameExpression(child, owner);
  }
  for (auto& element : expression->elements) {
    NameExpression(element.get(), owner);
  }
  for (auto& argument : expression->arguments) {
    NameExpression(argument.value.get(), owner);
  }
  for (auto& arm : expression->arms) {
    NameExpression(arm.pattern.get(), owner);
    NameExpression(arm.value.get(), owner);
  }
}

void NameStatement(frontend::Statement* statement, const std::string& owner) {
  if (statement == nullptr) {
    return;
  }
  for (auto* expression :
       {statement->expression.get(), statement->condition.get(), statement->increment.get()}) {
    NameExpression(expression, owner);
  }
  for (auto* child : {
           statement->then_branch.get(),
           statement->else_branch.get(),
           statement->initializer.get(),
           statement->finally_branch.get(),
       }) {
    NameStatement(child, owner);
  }
  for (auto& child : statement->statements) {
    NameStatement(child.get(), owner);
  }
  for (auto& branch : statement->cases) {
    NameExpression(branch.label.get(), owner);
    NameStatement(branch.body.get(), owner);
  }
  for (auto& caught : statement->catches) {
    NameStatement(caught.body.get(), owner);
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
