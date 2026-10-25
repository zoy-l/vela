#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
  // token 存储必须覆盖 Parse；语法角色直接标注到同一 token 流，供格式化等工具消费。
  explicit Parser(std::span<Token> tokens) : tokens_(tokens) {}

  [[nodiscard]] auto Parse() -> ParseResult;

 private:
  [[nodiscard]] auto Peek() const -> const Token&;
  [[nodiscard]] auto Previous() const -> const Token&;
  [[nodiscard]] auto IsAtEnd() const -> bool;
  [[nodiscard]] auto Check(TokenKind kind) const -> bool;
  auto Match(TokenKind kind) -> bool;
  auto Expect(TokenKind kind, std::string message) -> std::optional<Token>;
  void AnnotatePrevious(TokenRole role);
  void AddDiagnostic(SourceLocation location, std::string message);
  void Synchronize();
  void ParseDeclaration(Program& program);

  struct MemberModifiers final {
    Visibility visibility = Visibility::Public;
    MemberStorage storage = MemberStorage::Instance;
    FieldRole role = FieldRole::Ordinary;
  };
  auto ParseMemberModifiers() -> MemberModifiers;
  enum class MemberParseResult : std::uint8_t { Parsed, Failed };
  auto ParseClassMember(Class& declaration) -> MemberParseResult;
  auto ParseConstructor(Class& declaration, MemberModifiers modifiers) -> std::unique_ptr<Function>;
  auto ParseField(const Token& name, MemberModifiers modifiers) -> std::optional<Field>;

  auto ParseEnum() -> std::optional<Enum>;
  auto ParseImport() -> std::optional<Program::Import>;
  auto ParseClass() -> std::optional<Class>;
  auto ParseInterface() -> std::optional<Interface>;
  auto ParseFunction() -> std::optional<Function>;
  auto ParseTry() -> std::unique_ptr<Statement>;
  auto ParseThrow() -> std::unique_ptr<Statement>;
  auto ParseFunctionBody(const Token& name, std::string native_symbol = {})
      -> std::optional<Function>;
  auto ParseParameter() -> std::optional<Parameter>;
  auto ParseType() -> std::optional<TypeName>;
  auto ParseTypeBody() -> std::optional<TypeName>;
  auto ParseQualifiedTypeName() -> std::optional<std::string>;
  auto ParseParenthesizedType() -> std::optional<TypeName>;
  auto ParseTypeAlias() -> std::optional<Program::TypeAlias>;
  auto ParseTypeParameters() -> std::vector<std::string>;
  auto ParseTypeArguments() -> std::vector<TypeName>;
  [[nodiscard]] auto IsTypeApplication() const -> bool;
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
  auto ParseArrayLiteral() -> std::unique_ptr<Expression>;
  auto ParseLambda() -> std::unique_ptr<Expression>;
  auto ParseMatch() -> std::unique_ptr<Expression>;
  auto ParseDoubleLiteral() -> std::unique_ptr<Expression>;
  auto ParseString() -> std::unique_ptr<Expression>;
  auto ParseArguments() -> std::vector<CallArgument>;
  static auto MakeExpression(Expression::Kind kind, SourceLocation location)
      -> std::unique_ptr<Expression>;

  std::span<Token> tokens_;
  std::size_t current_ = 0;
  Diagnostics diagnostics_;
  std::size_t type_parse_depth_ = 0;
  std::size_t last_type_depth_ = 0;
  std::size_t last_type_arguments_depth_ = 0;
};

}  // namespace vela::frontend
