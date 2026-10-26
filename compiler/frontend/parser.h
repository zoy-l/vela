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

[[nodiscard]] ParseResult ParseSource(std::string_view source, std::uint32_t source_id = 0);
void AppendProgram(Program& destination, Program source);

class Parser final {
 public:
  // token 存储必须覆盖 Parse；语法角色直接标注到同一 token 流，供格式化等工具消费。
  explicit Parser(std::span<Token> tokens) : tokens_(tokens) {}

  [[nodiscard]] ParseResult Parse();

 private:
  [[nodiscard]] const Token& Peek() const;
  [[nodiscard]] const Token& Previous() const;
  [[nodiscard]] bool IsAtEnd() const;
  [[nodiscard]] bool Check(TokenKind kind) const;
  bool Match(TokenKind kind);
  std::optional<Token> Expect(TokenKind kind, std::string message);
  void AnnotatePrevious(TokenRole role);
  void AddDiagnostic(SourceLocation location, std::string message);
  void Synchronize();
  void ParseDeclaration(Program& program);

  struct MemberModifiers final {
    Visibility visibility = Visibility::Public;
    MemberStorage storage = MemberStorage::Instance;
    FieldRole role = FieldRole::Ordinary;
    bool is_override = false;
    bool is_getter = false;
    bool is_setter = false;
  };
  MemberModifiers ParseMemberModifiers();
  enum class MemberParseResult : std::uint8_t { Parsed, Failed };
  MemberParseResult ParseClassMember(Class& declaration);
  std::unique_ptr<Function> ParseConstructor(Class& declaration, MemberModifiers modifiers);
  std::optional<Field> ParseField(const Token& name, MemberModifiers modifiers);

  std::optional<Enum> ParseEnum();
  std::optional<Program::Import> ParseImport();
  std::optional<Class> ParseClass();
  std::optional<Interface> ParseInterface();
  std::optional<Function> ParseFunction();
  std::unique_ptr<Statement> ParseTry();
  std::unique_ptr<Statement> ParseThrow();
  std::optional<Function> ParseFunctionBody(const Token& name,
                                            std::string native_symbol = {},
                                            bool is_async = false);
  enum class ParameterTyping : std::uint8_t { Required, Inferred };
  std::optional<Parameter> ParseParameter(ParameterTyping typing = ParameterTyping::Required);
  std::optional<TypeName> ParseType();
  std::optional<TypeName> ParseTypeBody();
  std::optional<TypeName> ParseTypeSuffixes(TypeName result, std::size_t depth);
  std::optional<std::string> ParseQualifiedTypeName();
  std::optional<TypeName> ParseParenthesizedType();
  std::optional<Program::TypeAlias> ParseTypeAlias();
  std::vector<std::string> ParseTypeParameters();
  std::vector<TypeName> ParseTypeArguments();
  [[nodiscard]] bool IsTypeApplication() const;
  bool MatchTypeGreater();
  [[nodiscard]] bool IsVariableDeclaration() const;
  std::unique_ptr<Statement> ParseBlock();
  std::unique_ptr<Statement> ParseStatement();
  std::unique_ptr<Statement> ParseVariableDeclaration();
  std::unique_ptr<Statement> ParseReturnStatement();
  std::unique_ptr<Statement> ParseExpressionStatement();
  std::unique_ptr<Statement> ParseIfStatement();
  std::unique_ptr<Statement> ParseWhileStatement();
  std::unique_ptr<Statement> ParseForStatement();
  std::unique_ptr<Statement> ParseForEachStatement();
  std::unique_ptr<Statement> ParseBreakStatement();
  std::unique_ptr<Statement> ParseContinueStatement();
  std::unique_ptr<Statement> ParseSwitchStatement();

  std::unique_ptr<Expression> ParseExpression();
  std::unique_ptr<Expression> ParseAssignment();
  std::unique_ptr<Expression> ParseConditional();
  std::unique_ptr<Expression> ParseEquality();
  std::unique_ptr<Expression> ParseLogicalOr();
  std::unique_ptr<Expression> ParseLogicalAnd();
  std::unique_ptr<Expression> ParseBitwiseOr();
  std::unique_ptr<Expression> ParseBitwiseXor();
  std::unique_ptr<Expression> ParseBitwiseAnd();
  std::unique_ptr<Expression> ParseRelational();
  std::unique_ptr<Expression> ParseShift();
  std::unique_ptr<Expression> ParseAdditive();
  std::unique_ptr<Expression> ParseMultiplicative();
  std::unique_ptr<Expression> ParseUnary();
  std::unique_ptr<Expression> ParsePostfix();
  bool ParsePostfixTypeApplication(std::unique_ptr<Expression>& expression);
  bool ParsePostfixCall(std::unique_ptr<Expression>& expression);
  bool ParsePostfixMember(std::unique_ptr<Expression>& expression);
  bool ParsePostfixIndex(std::unique_ptr<Expression>& expression);
  bool ParsePostfixIncrement(std::unique_ptr<Expression>& expression);
  std::unique_ptr<Expression> ParsePrimary();
  [[nodiscard]] bool IsLambda() const;
  std::unique_ptr<Expression> ParseArrayLiteral();
  enum class LambdaParameters : std::uint8_t { Parenthesized, Single };
  std::unique_ptr<Expression> ParseLambda(LambdaParameters parameters, bool is_async = false);
  std::unique_ptr<Expression> ParseMatch();
  std::unique_ptr<Expression> ParseDoubleLiteral();
  std::unique_ptr<Expression> ParseString();
  std::vector<CallArgument> ParseArguments();
  static std::unique_ptr<Expression> MakeExpression(Expression::Kind kind, SourceLocation location);

  std::span<Token> tokens_;
  std::size_t current_ = 0;
  std::size_t pending_type_greater_ = 0;
  Diagnostics diagnostics_;
  std::size_t type_parse_depth_ = 0;
  std::size_t last_type_depth_ = 0;
  std::size_t last_type_arguments_depth_ = 0;
};

}  // namespace vela::frontend
