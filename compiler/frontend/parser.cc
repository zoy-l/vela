#include "compiler/frontend/parser.h"

#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include "compiler/frontend/ast.h"
#include "compiler/frontend/token.h"

namespace vela::frontend {

auto Parser::Parse() -> ParseResult {
  Program program;
  while (!IsAtEnd()) {
    std::optional<Enum> enum_declaration;
    std::optional<Class> class_declaration;
    std::optional<Function> function_declaration;
    bool handled_import = false;
    if (Match(TokenKind::Import)) {
      handled_import = true;
      const std::optional<std::string> path = ParseImport();
      if (path.has_value()) {
        program.imports.push_back(*path);
      }
    } else if (Match(TokenKind::Enum)) {
      enum_declaration = ParseEnum();
      if (enum_declaration.has_value()) {
        program.enums.push_back(std::move(enum_declaration.value()));
      }
    } else if (Match(TokenKind::Class)) {
      class_declaration = ParseClass();
      if (class_declaration.has_value()) {
        program.classes.push_back(std::move(class_declaration.value()));
      }
    } else {
      function_declaration = ParseFunction();
      if (function_declaration.has_value()) {
        program.functions.push_back(std::move(function_declaration.value()));
      }
    }
    if (!handled_import && !enum_declaration.has_value() && !class_declaration.has_value() &&
        !function_declaration.has_value()) {
      Synchronize();
    }
  }
  return {.program = std::move(program), .diagnostics = std::move(diagnostics_)};
}

auto Parser::ParseImport() -> std::optional<std::string> {
  if (!Match(TokenKind::StringLiteral)) {
    AddDiagnostic(Peek().location, "expected string path after import");
    return std::nullopt;
  }
  const Token path = Previous();
  if (!Expect(TokenKind::Semicolon, "expected ';' after import").has_value()) {
    return std::nullopt;
  }
  std::string value = path.lexeme;
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

auto Parser::Peek() const -> const Token& {
  return tokens_.at(current_);
}

auto Parser::Previous() const -> const Token& {
  return tokens_.at(current_ - 1);
}

auto Parser::IsAtEnd() const -> bool {
  return Check(TokenKind::EndOfFile);
}

auto Parser::Check(TokenKind kind) const -> bool {
  return Peek().kind == kind;
}

auto Parser::Match(TokenKind kind) -> bool {
  if (!Check(kind)) {
    return false;
  }
  ++current_;
  return true;
}

auto Parser::Expect(TokenKind kind, std::string message) -> std::optional<Token> {
  if (Check(kind)) {
    return tokens_.at(current_++);
  }
  AddDiagnostic(Peek().location, std::move(message));
  return std::nullopt;
}

void Parser::AddDiagnostic(SourceLocation location, std::string message) {
  diagnostics_.push_back({.location = location, .message = std::move(message)});
}

void Parser::Synchronize() {
  if (!IsAtEnd()) {
    ++current_;
  }
  while (!IsAtEnd()) {
    if (Previous().kind == TokenKind::Semicolon || Check(TokenKind::Class) ||
        Check(TokenKind::Enum) || Check(TokenKind::If) || Check(TokenKind::Return) ||
        Check(TokenKind::While) || Check(TokenKind::Func) || Check(TokenKind::Let) ||
        Check(TokenKind::Const)) {
      return;
    }
    ++current_;
  }
}

auto Parser::ParseEnum() -> std::optional<Enum> {
  const Token name = Peek();
  const std::optional<Token> name_token = Expect(TokenKind::Identifier, "expected enum name");
  if (!name_token.has_value()) {
    return std::nullopt;
  }
  if (!Expect(TokenKind::LeftBrace, "expected '{' after enum name").has_value()) {
    return std::nullopt;
  }

  Enum declaration;
  declaration.name = name_token->lexeme;
  declaration.location = name.location;
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    const std::optional<Token> member = Expect(TokenKind::Identifier, "expected enum member name");
    if (!member.has_value()) {
      return std::nullopt;
    }
    declaration.members.push_back(member->lexeme);
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (!Expect(TokenKind::RightBrace, "expected '}' after enum members").has_value()) {
    return std::nullopt;
  }
  return declaration;
}

auto Parser::ParseClass() -> std::optional<Class> {
  const auto name = Expect(TokenKind::Identifier, "expected class name");
  if (!name || !Expect(TokenKind::LeftBrace, "expected '{' after class name")) {
    return std::nullopt;
  }
  Class declaration;
  declaration.name = name->lexeme;
  declaration.location = name->location;
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    const auto member = Expect(TokenKind::Identifier, "expected field or method name");
    if (!member) {
      return std::nullopt;
    }
    if (Check(TokenKind::LeftParen)) {
      auto method = ParseFunctionBody(*member);
      if (!method) {
        return std::nullopt;
      }
      declaration.methods.push_back(std::move(*method));
    } else {
      if (!Expect(TokenKind::Colon, "expected ':' after field name")) {
        return std::nullopt;
      }
      const auto type = ParseType();
      if (!type || !Expect(TokenKind::Semicolon, "expected ';' after field declaration")) {
        return std::nullopt;
      }
      declaration.fields.push_back({
          .type = *type,
          .name = member->lexeme,
          .location = member->location,
      });
    }
  }
  if (!Expect(TokenKind::RightBrace, "expected '}' after class body")) {
    return std::nullopt;
  }
  return declaration;
}

auto Parser::ParseFunction() -> std::optional<Function> {
  if (!Expect(TokenKind::Func, "expected 'func' before function declaration")) {
    return std::nullopt;
  }
  const auto name = Expect(TokenKind::Identifier, "expected function name");
  return name ? ParseFunctionBody(*name) : std::nullopt;
}

auto Parser::ParseFunctionBody(const Token& name) -> std::optional<Function> {
  if (!Expect(TokenKind::LeftParen, "expected '(' after function name")) {
    return std::nullopt;
  }
  Function function;
  function.name = name.lexeme;
  function.location = name.location;
  while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
    const auto parameter = ParseParameter();
    if (!parameter) {
      return std::nullopt;
    }
    function.parameters.push_back(*parameter);
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (!Expect(TokenKind::RightParen, "expected ')' after parameters") ||
      !Expect(TokenKind::Colon, "expected ':' before return type")) {
    return std::nullopt;
  }
  const auto type = ParseType();
  if (!type) {
    return std::nullopt;
  }
  function.return_type = *type;
  function.body = ParseBlock();
  return function.body ? std::optional<Function>(std::move(function)) : std::nullopt;
}

auto Parser::ParseParameter() -> std::optional<Parameter> {
  const auto name = Expect(TokenKind::Identifier, "expected parameter name");
  if (!name || !Expect(TokenKind::Colon, "expected ':' after parameter name")) {
    return std::nullopt;
  }
  const auto type = ParseType();
  if (!type) {
    return std::nullopt;
  }
  return Parameter{.type = *type, .name = name->lexeme, .location = name->location};
}

auto Parser::ParseType() -> std::optional<TypeName> {
  if (Check(TokenKind::Identifier) || Check(TokenKind::Void)) {
    const Token token = tokens_.at(current_++);
    std::string name = token.kind == TokenKind::Void ? "void" : token.lexeme;
    while (Match(TokenKind::LeftBracket)) {
      if (!Expect(TokenKind::RightBracket, "expected ']' in array type").has_value()) {
        return std::nullopt;
      }
      name += "[]";
    }
    return TypeName{.name = std::move(name), .location = token.location};
  }
  AddDiagnostic(Peek().location, "expected type name");
  return std::nullopt;
}

auto Parser::ParseBlock() -> std::unique_ptr<Statement> {
  const std::optional<Token> left_brace = Expect(TokenKind::LeftBrace, "expected '{'");
  if (!left_brace.has_value()) {
    return nullptr;
  }
  auto block = std::make_unique<Statement>();
  block->kind = Statement::Kind::Block;
  block->location = left_brace->location;
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    std::unique_ptr<Statement> statement = ParseStatement();
    if (statement) {
      block->statements.push_back(std::move(statement));
    } else {
      Synchronize();
    }
  }
  if (!Expect(TokenKind::RightBrace, "expected '}' after block").has_value()) {
    return nullptr;
  }
  return block;
}

auto Parser::IsVariableDeclaration() const -> bool {
  return Check(TokenKind::Let) || Check(TokenKind::Const);
}

auto Parser::ParseStatement() -> std::unique_ptr<Statement> {
  if (Check(TokenKind::LeftBrace)) {
    return ParseBlock();
  }
  if (Match(TokenKind::If)) {
    return ParseIfStatement();
  }
  if (Match(TokenKind::Return)) {
    return ParseReturnStatement();
  }
  if (Match(TokenKind::While)) {
    return ParseWhileStatement();
  }
  if (Match(TokenKind::For)) {
    return ParseForStatement();
  }
  if (Match(TokenKind::Break)) {
    return ParseBreakStatement();
  }
  if (Match(TokenKind::Continue)) {
    return ParseContinueStatement();
  }
  if (IsVariableDeclaration()) {
    return ParseVariableDeclaration();
  }
  return ParseExpressionStatement();
}

auto Parser::ParseForStatement() -> std::unique_ptr<Statement> {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::For;
  statement->location = Previous().location;
  if (!Expect(TokenKind::LeftParen, "expected '(' after for").has_value()) {
    return nullptr;
  }
  if (!Check(TokenKind::Semicolon)) {
    if (IsVariableDeclaration()) {
      statement->initializer = ParseVariableDeclaration();
    } else {
      statement->initializer = ParseExpressionStatement();
    }
    if (!statement->initializer) {
      return nullptr;
    }
  } else {
    ++current_;
  }
  if (!Check(TokenKind::Semicolon)) {
    statement->condition = ParseExpression();
    if (!statement->condition) {
      return nullptr;
    }
  }
  if (!Expect(TokenKind::Semicolon, "expected ';' after for condition").has_value()) {
    return nullptr;
  }
  if (!Check(TokenKind::RightParen)) {
    statement->increment = ParseExpression();
    if (!statement->increment) {
      return nullptr;
    }
  }
  if (!Expect(TokenKind::RightParen, "expected ')' after for clauses").has_value()) {
    return nullptr;
  }
  statement->then_branch = ParseStatement();
  return statement->then_branch ? std::move(statement) : nullptr;
}

auto Parser::ParseBreakStatement() -> std::unique_ptr<Statement> {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::Break;
  statement->location = Previous().location;
  if (!Expect(TokenKind::Semicolon, "expected ';' after break").has_value()) {
    return nullptr;
  }
  return statement;
}

auto Parser::ParseContinueStatement() -> std::unique_ptr<Statement> {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::Continue;
  statement->location = Previous().location;
  if (!Expect(TokenKind::Semicolon, "expected ';' after continue").has_value()) {
    return nullptr;
  }
  return statement;
}

auto Parser::ParseIfStatement() -> std::unique_ptr<Statement> {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::If;
  statement->location = Previous().location;
  if (!Expect(TokenKind::LeftParen, "expected '(' after if").has_value()) {
    return nullptr;
  }
  statement->condition = ParseExpression();
  if (!statement->condition ||
      !Expect(TokenKind::RightParen, "expected ')' after if condition").has_value()) {
    return nullptr;
  }
  statement->then_branch = ParseStatement();
  if (!statement->then_branch) {
    return nullptr;
  }
  if (Match(TokenKind::Else)) {
    statement->else_branch = ParseStatement();
    if (!statement->else_branch) {
      return nullptr;
    }
  }
  return statement;
}

auto Parser::ParseWhileStatement() -> std::unique_ptr<Statement> {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::While;
  statement->location = Previous().location;
  if (!Expect(TokenKind::LeftParen, "expected '(' after while").has_value()) {
    return nullptr;
  }
  statement->condition = ParseExpression();
  if (!statement->condition ||
      !Expect(TokenKind::RightParen, "expected ')' after while condition").has_value()) {
    return nullptr;
  }
  statement->then_branch = ParseStatement();
  return statement->then_branch ? std::move(statement) : nullptr;
}

auto Parser::ParseVariableDeclaration() -> std::unique_ptr<Statement> {
  const Token keyword = tokens_.at(current_++);
  const auto name = Expect(TokenKind::Identifier, "expected variable name");
  if (!name) {
    return nullptr;
  }
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::VariableDeclaration;
  statement->location = name->location;
  statement->name = name->lexeme;
  statement->binding =
      keyword.kind == TokenKind::Const ? BindingKind::Constant : BindingKind::Mutable;
  if (Match(TokenKind::Colon)) {
    const auto type = ParseType();
    if (!type) {
      return nullptr;
    }
    statement->type = *type;
  }
  if (!Expect(TokenKind::Equal, "variable declarations require an initializer")) {
    return nullptr;
  }
  statement->expression = ParseExpression();
  if (!statement->expression ||
      !Expect(TokenKind::Semicolon, "expected ';' after variable declaration")) {
    return nullptr;
  }
  return statement;
}

auto Parser::ParseReturnStatement() -> std::unique_ptr<Statement> {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::Return;
  statement->location = Previous().location;
  if (!Check(TokenKind::Semicolon)) {
    statement->expression = ParseExpression();
    if (!statement->expression) {
      return nullptr;
    }
  }
  if (!Expect(TokenKind::Semicolon, "expected ';' after return statement").has_value()) {
    return nullptr;
  }
  return statement;
}

auto Parser::ParseExpressionStatement() -> std::unique_ptr<Statement> {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::Expression;
  statement->location = Peek().location;
  statement->expression = ParseExpression();
  if (!statement->expression) {
    return nullptr;
  }
  if (!Expect(TokenKind::Semicolon, "expected ';' after expression").has_value()) {
    return nullptr;
  }
  return statement;
}

auto Parser::ParseExpression() -> std::unique_ptr<Expression> {
  return ParseAssignment();
}

auto Parser::ParseAssignment() -> std::unique_ptr<Expression> {
  std::unique_ptr<Expression> expression = ParseLogicalOr();
  if (Match(TokenKind::Equal)) {
    const SourceLocation location = Previous().location;
    std::unique_ptr<Expression> right = ParseAssignment();
    if (!right) {
      return nullptr;
    }
    auto assignment = MakeExpression(Expression::Kind::Binary, location);
    assignment->text = "=";
    assignment->left = std::move(expression);
    assignment->right = std::move(right);
    return assignment;
  }
  return expression;
}

auto Parser::ParseLogicalOr() -> std::unique_ptr<Expression> {
  std::unique_ptr<Expression> expression = ParseLogicalAnd();
  while (Match(TokenKind::OrOr)) {
    const Token operator_token = Previous();
    auto right = ParseLogicalAnd();
    if (!right) {
      return nullptr;
    }
    auto binary = MakeExpression(Expression::Kind::Binary, operator_token.location);
    binary->text = operator_token.lexeme;
    binary->left = std::move(expression);
    binary->right = std::move(right);
    expression = std::move(binary);
  }
  return expression;
}

auto Parser::ParseLogicalAnd() -> std::unique_ptr<Expression> {
  std::unique_ptr<Expression> expression = ParseEquality();
  while (Match(TokenKind::AndAnd)) {
    const Token operator_token = Previous();
    auto right = ParseEquality();
    if (!right) {
      return nullptr;
    }
    auto binary = MakeExpression(Expression::Kind::Binary, operator_token.location);
    binary->text = operator_token.lexeme;
    binary->left = std::move(expression);
    binary->right = std::move(right);
    expression = std::move(binary);
  }
  return expression;
}

auto Parser::ParseEquality() -> std::unique_ptr<Expression> {
  std::unique_ptr<Expression> expression = ParseRelational();
  while (Match(TokenKind::EqualEqual) || Match(TokenKind::BangEqual)) {
    const Token operator_token = Previous();
    std::unique_ptr<Expression> right = ParseRelational();
    if (!right) {
      return nullptr;
    }
    auto binary = MakeExpression(Expression::Kind::Binary, operator_token.location);
    binary->text = operator_token.lexeme;
    binary->left = std::move(expression);
    binary->right = std::move(right);
    expression = std::move(binary);
  }
  return expression;
}

auto Parser::ParseRelational() -> std::unique_ptr<Expression> {
  std::unique_ptr<Expression> expression = ParseAdditive();
  while (Match(TokenKind::Less) || Match(TokenKind::LessEqual) || Match(TokenKind::Greater) ||
         Match(TokenKind::GreaterEqual)) {
    const Token operator_token = Previous();
    auto right = ParseAdditive();
    if (!right) {
      return nullptr;
    }
    auto binary = MakeExpression(Expression::Kind::Binary, operator_token.location);
    binary->text = operator_token.lexeme;
    binary->left = std::move(expression);
    binary->right = std::move(right);
    expression = std::move(binary);
  }
  return expression;
}

auto Parser::ParseAdditive() -> std::unique_ptr<Expression> {
  std::unique_ptr<Expression> expression = ParseMultiplicative();
  while (Match(TokenKind::Plus) || Match(TokenKind::Minus)) {
    const Token operator_token = Previous();
    std::unique_ptr<Expression> right = ParseMultiplicative();
    if (!right) {
      return nullptr;
    }
    auto binary = MakeExpression(Expression::Kind::Binary, operator_token.location);
    binary->text = operator_token.lexeme;
    binary->left = std::move(expression);
    binary->right = std::move(right);
    expression = std::move(binary);
  }
  return expression;
}

auto Parser::ParseMultiplicative() -> std::unique_ptr<Expression> {
  std::unique_ptr<Expression> expression = ParseUnary();
  while (Match(TokenKind::Star) || Match(TokenKind::Slash)) {
    const Token operator_token = Previous();
    std::unique_ptr<Expression> right = ParseUnary();
    if (!right) {
      return nullptr;
    }
    auto binary = MakeExpression(Expression::Kind::Binary, operator_token.location);
    binary->text = operator_token.lexeme;
    binary->left = std::move(expression);
    binary->right = std::move(right);
    expression = std::move(binary);
  }
  return expression;
}

auto Parser::ParseUnary() -> std::unique_ptr<Expression> {
  if (Match(TokenKind::Bang) || Match(TokenKind::Minus)) {
    const Token operator_token = Previous();
    auto unary = MakeExpression(Expression::Kind::Unary, operator_token.location);
    unary->text = operator_token.lexeme;
    unary->operand = ParseUnary();
    return unary->operand ? std::move(unary) : nullptr;
  }
  return ParsePostfix();
}

auto Parser::ParsePostfix() -> std::unique_ptr<Expression> {
  std::unique_ptr<Expression> expression = ParsePrimary();
  if (!expression) {
    return nullptr;
  }
  while (true) {
    if (Match(TokenKind::LeftParen)) {
      auto call = MakeExpression(Expression::Kind::Call, expression->location);
      call->callee = std::move(expression);
      call->arguments = ParseArguments();
      if (!Expect(TokenKind::RightParen, "expected ')' after arguments").has_value()) {
        return nullptr;
      }
      expression = std::move(call);
    } else if (Match(TokenKind::Dot)) {
      const std::optional<Token> member =
          Expect(TokenKind::Identifier, "expected member name after '.'");
      if (!member.has_value()) {
        return nullptr;
      }
      auto access = MakeExpression(Expression::Kind::Member, member->location);
      access->object = std::move(expression);
      access->text = member->lexeme;
      expression = std::move(access);
    } else if (Match(TokenKind::LeftBracket)) {
      auto index = MakeExpression(Expression::Kind::Index, Previous().location);
      index->object = std::move(expression);
      index->index = ParseExpression();
      if (!index->index ||
          !Expect(TokenKind::RightBracket, "expected ']' after index").has_value()) {
        return nullptr;
      }
      expression = std::move(index);
    } else {
      break;
    }
  }
  return expression;
}

auto Parser::ParseDoubleLiteral() -> std::unique_ptr<Expression> {
  auto expression = MakeExpression(Expression::Kind::DoubleLiteral, Previous().location);
  const auto& text = Previous().lexeme;
  const auto parsed =
      std::from_chars(text.data(), std::to_address(text.end()), expression->double_value);
  if (parsed.ec != std::errc()) {
    AddDiagnostic(expression->location, "double literal is out of range");
    return nullptr;
  }
  return expression;
}

auto Parser::ParsePrimary() -> std::unique_ptr<Expression> {
  if (Match(TokenKind::IntegerLiteral)) {
    auto expression = MakeExpression(Expression::Kind::IntegerLiteral, Previous().location);
    expression->text = Previous().lexeme;
    return expression;
  }
  if (Match(TokenKind::DoubleLiteral)) {
    return ParseDoubleLiteral();
  }
  if (Match(TokenKind::StringLiteral)) {
    auto expression = MakeExpression(Expression::Kind::StringLiteral, Previous().location);
    expression->text = Previous().lexeme;
    return expression;
  }
  if (Match(TokenKind::LeftBracket)) {
    auto expression = MakeExpression(Expression::Kind::ArrayLiteral, Previous().location);
    while (!Check(TokenKind::RightBracket) && !IsAtEnd()) {
      auto element = ParseExpression();
      if (!element) {
        return nullptr;
      }
      expression->elements.push_back(std::move(element));
      if (!Match(TokenKind::Comma)) {
        break;
      }
    }
    if (!Expect(TokenKind::RightBracket, "expected ']' after array literal").has_value()) {
      return nullptr;
    }
    return expression;
  }
  if (Match(TokenKind::True) || Match(TokenKind::False)) {
    auto expression = MakeExpression(Expression::Kind::BooleanLiteral, Previous().location);
    expression->boolean_value = Previous().kind == TokenKind::True;
    return expression;
  }
  if (Match(TokenKind::Identifier) || Match(TokenKind::Print)) {
    auto expression = MakeExpression(Expression::Kind::Name, Previous().location);
    expression->text = Previous().lexeme;
    return expression;
  }
  if (Match(TokenKind::Dot)) {
    const std::optional<Token> member =
        Expect(TokenKind::Identifier, "expected member name after '.'");
    if (!member.has_value()) {
      return nullptr;
    }
    auto expression = MakeExpression(Expression::Kind::ShorthandMember, member->location);
    expression->text = member->lexeme;
    return expression;
  }
  if (Match(TokenKind::LeftParen)) {
    std::unique_ptr<Expression> expression = ParseExpression();
    if (!Expect(TokenKind::RightParen, "expected ')' after expression").has_value()) {
      return nullptr;
    }
    return expression;
  }

  AddDiagnostic(Peek().location, "expected expression");
  return nullptr;
}

auto Parser::ParseArguments() -> std::vector<CallArgument> {
  std::vector<CallArgument> arguments;
  while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
    CallArgument argument;
    argument.location = Peek().location;
    if (Check(TokenKind::Identifier) && current_ + 1 < tokens_.size() &&
        tokens_.at(current_ + 1).kind == TokenKind::Colon) {
      argument.name = tokens_.at(current_++).lexeme;
      ++current_;
    }
    argument.value = ParseExpression();
    if (!argument.value) {
      return {};
    }
    arguments.push_back(std::move(argument));
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  return arguments;
}

auto Parser::MakeExpression(Expression::Kind kind, SourceLocation location)
    -> std::unique_ptr<Expression> {
  auto expression = std::make_unique<Expression>();
  expression->kind = kind;
  expression->location = location;
  return expression;
}

}  // namespace vela::frontend
