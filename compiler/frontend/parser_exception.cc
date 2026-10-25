#include "compiler/frontend/parser.h"

#include <memory>
#include <utility>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/token.h"

namespace vela::frontend {

auto Parser::ParseTry() -> std::unique_ptr<Statement> {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::Try;
  statement->location = Previous().location;
  statement->then_branch = ParseBlock();
  if (!statement->then_branch) {
    return nullptr;
  }
  bool catch_all = false;
  while (Match(TokenKind::Catch)) {
    Statement::Catch clause;
    clause.location = Previous().location;
    if (catch_all) {
      AddDiagnostic(clause.location, "catch-all must be the last catch clause");
      return nullptr;
    }
    if (Match(TokenKind::LeftParen)) {
      const auto parameter = ParseParameter();
      if (!parameter || !Expect(TokenKind::RightParen, "expected ')' after catch parameter")) {
        return nullptr;
      }
      clause.name = parameter->name;
      clause.type = parameter->type;
    } else {
      catch_all = true;
    }
    clause.body = ParseBlock();
    if (!clause.body) {
      return nullptr;
    }
    statement->catches.push_back(std::move(clause));
  }
  if (Match(TokenKind::Finally)) {
    statement->finally_branch = ParseBlock();
    if (!statement->finally_branch) {
      return nullptr;
    }
  }
  if (statement->catches.empty() && !statement->finally_branch) {
    AddDiagnostic(statement->location, "try requires catch or finally");
    return nullptr;
  }
  return statement;
}

auto Parser::ParseThrow() -> std::unique_ptr<Statement> {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::Throw;
  statement->location = Previous().location;
  statement->expression = ParseExpression();
  if (!statement->expression || !Expect(TokenKind::Semicolon, "expected ';' after throw")) {
    return nullptr;
  }
  return statement;
}

}  // namespace vela::frontend
