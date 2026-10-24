#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/token.h"

namespace vela::frontend {

struct ParseResult final {
  Program program;
  Diagnostics diagnostics;
};

[[nodiscard]] auto ParseSource(std::string_view source, std::uint32_t source_id = 0) -> ParseResult;
void AppendProgram(Program& destination, Program source);

class Parser final {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  [[nodiscard]] auto Parse() -> ParseResult;

 private:
  [[nodiscard]] auto Peek() const -> const Token&;
  [[nodiscard]] auto Previous() const -> const Token&;
  [[nodiscard]] auto IsAtEnd() const -> bool;
  [[nodiscard]] auto Check(TokenKind kind) const -> bool;
  auto Match(TokenKind kind) -> bool;
  auto Expect(TokenKind kind, std::string message) -> std::optional<Token>;
  void AddDiagnostic(SourceLocation location, std::string message);
  void Synchronize();

  auto ParseEnum() -> std::optional<Enum>;
  auto ParseImport() -> std::optional<Program::Import>;
  auto ParseClass() -> std::optional<Class>;
  auto ParseFunction() -> std::optional<Function>;
  auto ParseFunctionBody(const Token& name) -> std::optional<Function>;
  auto ParseParameter() -> std::optional<Parameter>;
  auto ParseType() -> std::optional<TypeName>;
  [[nodiscard]] auto IsVariableDeclaration() const -> bool;
  auto ParseBlock() -> std::unique_ptr<Statement>;
  auto ParseStatement() -> std::unique_ptr<Statement>;
  auto ParseVariableDeclaration() -> std::unique_ptr<Statement>;
  auto ParseReturnStatement() -> std::unique_ptr<Statement>;
  auto ParseExpressionStatement() -> std::unique_ptr<Statement>;
  auto ParseIfStatement() -> std::unique_ptr<Statement>;
  auto ParseWhileStatement() -> std::unique_ptr<Statement>;
  auto ParseForStatement() -> std::unique_ptr<Statement>;
  auto ParseBreakStatement() -> std::unique_ptr<Statement>;
  auto ParseContinueStatement() -> std::unique_ptr<Statement>;
  auto ParseSwitchStatement() -> std::unique_ptr<Statement>;

  auto ParseExpression() -> std::unique_ptr<Expression>;
  auto ParseAssignment() -> std::unique_ptr<Expression>;
  auto ParseEquality() -> std::unique_ptr<Expression>;
  auto ParseLogicalOr() -> std::unique_ptr<Expression>;
  auto ParseLogicalAnd() -> std::unique_ptr<Expression>;
  auto ParseRelational() -> std::unique_ptr<Expression>;
  auto ParseAdditive() -> std::unique_ptr<Expression>;
  auto ParseMultiplicative() -> std::unique_ptr<Expression>;
  auto ParseUnary() -> std::unique_ptr<Expression>;
  auto ParsePostfix() -> std::unique_ptr<Expression>;
  auto ParsePrimary() -> std::unique_ptr<Expression>;
  auto ParseDoubleLiteral() -> std::unique_ptr<Expression>;
  auto ParseString() -> std::unique_ptr<Expression>;
  auto ParseArguments() -> std::vector<CallArgument>;
  static auto MakeExpression(Expression::Kind kind, SourceLocation location)
      -> std::unique_ptr<Expression>;

  std::vector<Token> tokens_;
  std::size_t current_ = 0;
  Diagnostics diagnostics_;
};

}  // namespace vela::frontend
