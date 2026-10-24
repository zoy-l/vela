#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/token.h"

namespace vela::frontend {

struct ParseResult final {
  Program program;
  Diagnostics diagnostics;
};

class Parser final {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  [[nodiscard]] ParseResult Parse();

 private:
  [[nodiscard]] const Token& Peek() const;
  [[nodiscard]] const Token& Previous() const;
  [[nodiscard]] bool IsAtEnd() const;
  [[nodiscard]] bool Check(TokenKind kind) const;
  bool Match(TokenKind kind);
  std::optional<Token> Expect(TokenKind kind, std::string message);
  void AddDiagnostic(SourceLocation location, std::string message);
  void Synchronize();

  std::optional<Enum> ParseEnum();
  std::optional<Class> ParseClass();
  std::optional<Function> ParseFunction();
  std::optional<Parameter> ParseParameter();
  std::optional<TypeName> ParseType();
  std::unique_ptr<Statement> ParseBlock();
  std::unique_ptr<Statement> ParseStatement();
  std::unique_ptr<Statement> ParseVariableDeclaration();
  std::unique_ptr<Statement> ParseReturnStatement();
  std::unique_ptr<Statement> ParseExpressionStatement();

  std::unique_ptr<Expression> ParseExpression();
  std::unique_ptr<Expression> ParseAssignment();
  std::unique_ptr<Expression> ParseEquality();
  std::unique_ptr<Expression> ParseAdditive();
  std::unique_ptr<Expression> ParseMultiplicative();
  std::unique_ptr<Expression> ParseUnary();
  std::unique_ptr<Expression> ParsePostfix();
  std::unique_ptr<Expression> ParsePrimary();
  std::vector<CallArgument> ParseArguments();
  std::unique_ptr<Expression> MakeExpression(Expression::Kind kind, SourceLocation location);

  std::vector<Token> tokens_;
  std::size_t current_ = 0;
  Diagnostics diagnostics_;
};

}  // namespace vela::frontend
