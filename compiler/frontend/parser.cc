#include "compiler/frontend/parser.h"

#include <utility>

namespace vela::frontend {

ParseResult Parser::Parse() {
  Program program;
  while (!IsAtEnd()) {
    std::optional<Enum> enum_declaration;
    std::optional<Class> class_declaration;
    std::optional<Function> function_declaration;
    if (Match(TokenKind::Enum)) {
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
    if (!enum_declaration.has_value() && !class_declaration.has_value() &&
        !function_declaration.has_value()) {
      Synchronize();
    }
  }
  return {std::move(program), std::move(diagnostics_)};
}

const Token& Parser::Peek() const {
  return tokens_[current_];
}

const Token& Parser::Previous() const {
  return tokens_[current_ - 1];
}

bool Parser::IsAtEnd() const {
  return Check(TokenKind::EndOfFile);
}

bool Parser::Check(TokenKind kind) const {
  return Peek().kind == kind;
}

bool Parser::Match(TokenKind kind) {
  if (!Check(kind)) {
    return false;
  }
  ++current_;
  return true;
}

std::optional<Token> Parser::Expect(TokenKind kind, std::string message) {
  if (Check(kind)) {
    return tokens_[current_++];
  }
  AddDiagnostic(Peek().location, std::move(message));
  return std::nullopt;
}

void Parser::AddDiagnostic(SourceLocation location, std::string message) {
  diagnostics_.push_back({location, std::move(message)});
}

void Parser::Synchronize() {
  if (!IsAtEnd()) {
    ++current_;
  }
  while (!IsAtEnd()) {
    if (Previous().kind == TokenKind::Semicolon || Check(TokenKind::Class) ||
        Check(TokenKind::Enum) || Check(TokenKind::Return)) {
      return;
    }
    ++current_;
  }
}

std::optional<Enum> Parser::ParseEnum() {
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

std::optional<Class> Parser::ParseClass() {
  const std::optional<Token> name = Expect(TokenKind::Identifier, "expected class name");
  if (!name.has_value()) {
    return std::nullopt;
  }
  if (!Expect(TokenKind::LeftBrace, "expected '{' after class name").has_value()) {
    return std::nullopt;
  }

  Class declaration;
  declaration.name = name->lexeme;
  declaration.location = name->location;
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    const std::size_t member_start = current_;
    const std::optional<TypeName> type = ParseType();
    if (!type.has_value()) {
      return std::nullopt;
    }
    const std::optional<Token> member =
        Expect(TokenKind::Identifier, "expected field or method name");
    if (!member.has_value()) {
      return std::nullopt;
    }
    if (Match(TokenKind::LeftParen)) {
      Function method;
      method.name = member->lexeme;
      method.return_type = type.value();
      method.location = tokens_[member_start].location;
      while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
        const std::optional<Parameter> parameter = ParseParameter();
        if (!parameter.has_value()) {
          return std::nullopt;
        }
        method.parameters.push_back(parameter.value());
        if (!Match(TokenKind::Comma)) {
          break;
        }
      }
      if (!Expect(TokenKind::RightParen, "expected ')' after method parameters").has_value()) {
        return std::nullopt;
      }
      method.body = ParseBlock();
      if (!method.body) {
        return std::nullopt;
      }
      declaration.methods.push_back(std::move(method));
    } else {
      Field field;
      field.type = type.value();
      field.name = member->lexeme;
      field.location = member->location;
      if (!Expect(TokenKind::Semicolon, "expected ';' after field declaration").has_value()) {
        return std::nullopt;
      }
      declaration.fields.push_back(std::move(field));
    }
  }
  if (!Expect(TokenKind::RightBrace, "expected '}' after class body").has_value()) {
    return std::nullopt;
  }
  return declaration;
}

std::optional<Function> Parser::ParseFunction() {
  const std::optional<TypeName> return_type = ParseType();
  if (!return_type.has_value()) {
    return std::nullopt;
  }
  const std::optional<Token> name = Expect(TokenKind::Identifier, "expected function name");
  if (!name.has_value()) {
    return std::nullopt;
  }
  if (!Expect(TokenKind::LeftParen, "expected '(' after function name").has_value()) {
    return std::nullopt;
  }

  Function function;
  function.name = name->lexeme;
  function.return_type = return_type.value();
  function.location = name->location;
  while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
    const std::optional<Parameter> parameter = ParseParameter();
    if (!parameter.has_value()) {
      return std::nullopt;
    }
    function.parameters.push_back(parameter.value());
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (!Expect(TokenKind::RightParen, "expected ')' after function parameters").has_value()) {
    return std::nullopt;
  }
  function.body = ParseBlock();
  if (!function.body) {
    return std::nullopt;
  }
  return function;
}

std::optional<Parameter> Parser::ParseParameter() {
  const std::optional<TypeName> type = ParseType();
  if (!type.has_value()) {
    return std::nullopt;
  }
  const std::optional<Token> name = Expect(TokenKind::Identifier, "expected parameter name");
  if (!name.has_value()) {
    return std::nullopt;
  }
  return Parameter{type.value(), name->lexeme, name->location};
}

std::optional<TypeName> Parser::ParseType() {
  if (Check(TokenKind::Identifier) || Check(TokenKind::Void)) {
    const Token token = tokens_[current_++];
    return TypeName{token.kind == TokenKind::Void ? "void" : token.lexeme, token.location};
  }
  AddDiagnostic(Peek().location, "expected type name");
  return std::nullopt;
}

std::unique_ptr<Statement> Parser::ParseBlock() {
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

std::unique_ptr<Statement> Parser::ParseStatement() {
  if (Check(TokenKind::LeftBrace)) {
    return ParseBlock();
  }
  if (Match(TokenKind::Return)) {
    return ParseReturnStatement();
  }
  if (Check(TokenKind::Identifier) && current_ + 1 < tokens_.size() &&
      tokens_[current_ + 1].kind == TokenKind::Identifier) {
    return ParseVariableDeclaration();
  }
  return ParseExpressionStatement();
}

std::unique_ptr<Statement> Parser::ParseVariableDeclaration() {
  const std::optional<TypeName> type = ParseType();
  if (!type.has_value()) {
    return nullptr;
  }
  const std::optional<Token> name = Expect(TokenKind::Identifier, "expected variable name");
  if (!name.has_value()) {
    return nullptr;
  }
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::VariableDeclaration;
  statement->location = name->location;
  statement->type = type.value();
  statement->name = name->lexeme;
  if (Match(TokenKind::Equal)) {
    statement->expression = ParseExpression();
    if (!statement->expression) {
      return nullptr;
    }
  }
  if (!Expect(TokenKind::Semicolon, "expected ';' after variable declaration").has_value()) {
    return nullptr;
  }
  return statement;
}

std::unique_ptr<Statement> Parser::ParseReturnStatement() {
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

std::unique_ptr<Statement> Parser::ParseExpressionStatement() {
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

std::unique_ptr<Expression> Parser::ParseExpression() {
  return ParseAssignment();
}

std::unique_ptr<Expression> Parser::ParseAssignment() {
  std::unique_ptr<Expression> expression = ParseEquality();
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

std::unique_ptr<Expression> Parser::ParseEquality() {
  std::unique_ptr<Expression> expression = ParseAdditive();
  while (Match(TokenKind::EqualEqual) || Match(TokenKind::BangEqual)) {
    const Token operator_token = Previous();
    std::unique_ptr<Expression> right = ParseAdditive();
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

std::unique_ptr<Expression> Parser::ParseAdditive() {
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

std::unique_ptr<Expression> Parser::ParseMultiplicative() {
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

std::unique_ptr<Expression> Parser::ParseUnary() {
  if (Match(TokenKind::Bang) || Match(TokenKind::Minus)) {
    const Token operator_token = Previous();
    auto unary = MakeExpression(Expression::Kind::Unary, operator_token.location);
    unary->text = operator_token.lexeme;
    unary->operand = ParseUnary();
    return unary->operand ? std::move(unary) : nullptr;
  }
  return ParsePostfix();
}

std::unique_ptr<Expression> Parser::ParsePostfix() {
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
    } else {
      break;
    }
  }
  return expression;
}

std::unique_ptr<Expression> Parser::ParsePrimary() {
  if (Match(TokenKind::IntegerLiteral)) {
    auto expression = MakeExpression(Expression::Kind::IntegerLiteral, Previous().location);
    expression->text = Previous().lexeme;
    return expression;
  }
  if (Match(TokenKind::FloatLiteral)) {
    auto expression = MakeExpression(Expression::Kind::FloatLiteral, Previous().location);
    expression->text = Previous().lexeme;
    return expression;
  }
  if (Match(TokenKind::StringLiteral)) {
    auto expression = MakeExpression(Expression::Kind::StringLiteral, Previous().location);
    expression->text = Previous().lexeme;
    return expression;
  }
  if (Match(TokenKind::True) || Match(TokenKind::False)) {
    auto expression = MakeExpression(Expression::Kind::BooleanLiteral, Previous().location);
    expression->boolean_value = Previous().kind == TokenKind::True;
    return expression;
  }
  if (Match(TokenKind::Identifier)) {
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

std::vector<CallArgument> Parser::ParseArguments() {
  std::vector<CallArgument> arguments;
  while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
    CallArgument argument;
    argument.location = Peek().location;
    if (Check(TokenKind::Identifier) && current_ + 1 < tokens_.size() &&
        tokens_[current_ + 1].kind == TokenKind::Colon) {
      argument.name = tokens_[current_++].lexeme;
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

std::unique_ptr<Expression> Parser::MakeExpression(Expression::Kind kind, SourceLocation location) {
  auto expression = std::make_unique<Expression>();
  expression->kind = kind;
  expression->location = location;
  return expression;
}

}  // namespace vela::frontend
