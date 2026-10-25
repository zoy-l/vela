#include "compiler/frontend/parser.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "compiler/frontend/ast.h"
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/token.h"

namespace vela::frontend {

auto ParseSource(std::string_view source, std::uint32_t source_id) -> ParseResult {
  auto lexed = Lexer(source, source_id).Tokenize();
  if (!lexed.diagnostics.empty()) {
    return {.program = Program(), .diagnostics = std::move(lexed.diagnostics)};
  }
  return Parser(lexed.tokens).Parse();
}

void AppendProgram(Program& destination, Program source) {
  std::ranges::move(source.enums, std::back_inserter(destination.enums));
  std::ranges::move(source.classes, std::back_inserter(destination.classes));
  std::ranges::move(source.functions, std::back_inserter(destination.functions));
  std::ranges::move(source.imports, std::back_inserter(destination.imports));
  std::ranges::move(source.aliases, std::back_inserter(destination.aliases));
  std::ranges::move(source.interfaces, std::back_inserter(destination.interfaces));
  std::ranges::move(source.globals, std::back_inserter(destination.globals));
}

auto Parser::Parse() -> ParseResult {
  Program program;
  while (!IsAtEnd()) {
    ParseDeclaration(program);
  }
  return {.program = std::move(program), .diagnostics = std::move(diagnostics_)};
}

void Parser::ParseDeclaration(Program& program) {
  const auto export_kind = Match(TokenKind::Export) ? ExportKind::Exported : ExportKind::Local;
  const auto append = [this, export_kind](auto declaration, auto& destination) -> void {
    if (declaration) {
      declaration->export_kind = export_kind;
      destination.push_back(std::move(*declaration));
    } else {
      Synchronize();
    }
  };
  const auto kind = Peek().kind;
  if (kind != TokenKind::Func && kind != TokenKind::External) {
    ++current_;
  }
  switch (kind) {
    case TokenKind::External:
      append(ParseFunction(), program.functions);
      break;
    case TokenKind::Interface:
      append(ParseInterface(), program.interfaces);
      break;
    case TokenKind::Type:
      append(ParseTypeAlias(), program.aliases);
      break;
    case TokenKind::Import:
      if (export_kind == ExportKind::Exported) {
        AddDiagnostic(Previous().location, "export applies to declarations, not import directives");
      }
      if (auto imported = ParseImport()) {
        program.imports.push_back(std::move(*imported));
      } else {
        Synchronize();
      }
      break;
    case TokenKind::Enum:
      append(ParseEnum(), program.enums);
      break;
    case TokenKind::Class:
      append(ParseClass(), program.classes);
      break;
    case TokenKind::Let:
    case TokenKind::Const: {
      --current_;
      auto declaration = ParseVariableDeclaration();
      if (!declaration) {
        Synchronize();
        break;
      }
      program.globals.push_back({
          .name = std::move(declaration->name),
          .type = std::move(declaration->type),
          .initializer = std::move(declaration->expression),
          .location = declaration->location,
          .binding = declaration->binding,
          .export_kind = export_kind,
      });
      break;
    }
    default:
      if (kind != TokenKind::Func) {
        --current_;
      }
      append(ParseFunction(), program.functions);
      break;
  }
}

auto Parser::ParseImport() -> std::optional<Program::Import> {
  if (!Match(TokenKind::StringLiteral)) {
    AddDiagnostic(Peek().location, "expected string path after import");
    return std::nullopt;
  }
  const Token path = Previous();
  std::string alias;
  if (Match(TokenKind::As)) {
    const auto name = Expect(TokenKind::Identifier, "expected import prefix after 'as'");
    if (!name) {
      return std::nullopt;
    }
    alias = name->lexeme;
  }
  if (!Expect(TokenKind::Semicolon, "expected ';' after import").has_value()) {
    return std::nullopt;
  }
  if (path.lexeme.contains('\0')) {
    AddDiagnostic(path.location, "import path cannot contain a null byte");
    return std::nullopt;
  }
  return Program::Import{.path = path.lexeme, .location = path.location, .alias = std::move(alias)};
}

void Parser::AnnotatePrevious(TokenRole role) {
  tokens_.subspan(current_ - 1).front().role = role;
}

auto Parser::Peek() const -> const Token& {
  return tokens_.subspan(current_).front();
}

auto Parser::Previous() const -> const Token& {
  return tokens_.subspan(current_ - 1).front();
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
    return tokens_.subspan(current_++).front();
  }
  AddDiagnostic(Peek().location, std::move(message));
  return std::nullopt;
}

void Parser::AddDiagnostic(SourceLocation location, std::string message) {
  diagnostics_.push_back({.location = location, .message = std::move(message), .source_path = {}});
}

void Parser::Synchronize() {
  if (Check(TokenKind::Interface) || Check(TokenKind::Type) || Check(TokenKind::Func) ||
      Check(TokenKind::Class) || Check(TokenKind::Enum) || Check(TokenKind::Import) ||
      Check(TokenKind::External)) {
    return;
  }
  if (!IsAtEnd()) {
    ++current_;
  }
  while (!IsAtEnd()) {
    if (Previous().kind == TokenKind::Semicolon || Check(TokenKind::Class) ||
        Check(TokenKind::Enum) || Check(TokenKind::If) || Check(TokenKind::Return) ||
        Check(TokenKind::While) || Check(TokenKind::Func) || Check(TokenKind::Let) ||
        Check(TokenKind::Const) || Check(TokenKind::Export) || Check(TokenKind::External)) {
      return;
    }
    ++current_;
  }
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
    if (Check(TokenKind::Interface) || Check(TokenKind::Type) || Check(TokenKind::Func) ||
        Check(TokenKind::Class) || Check(TokenKind::Enum) || Check(TokenKind::Import) ||
        Check(TokenKind::Export) || Check(TokenKind::External)) {
      AddDiagnostic(Peek().location, "expected '}' before declaration");
      return nullptr;
    }
    const auto start = current_;
    std::unique_ptr<Statement> statement = ParseStatement();
    if (statement) {
      block->statements.push_back(std::move(statement));
    } else if (current_ == start || !Check(TokenKind::RightBrace)) {
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
  if (Match(TokenKind::Try)) {
    return ParseTry();
  }
  if (Match(TokenKind::Throw)) {
    return ParseThrow();
  }
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
  if (Match(TokenKind::Switch)) {
    return ParseSwitchStatement();
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

auto Parser::ParseSwitchStatement() -> std::unique_ptr<Statement> {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::Switch;
  statement->location = Previous().location;
  if (!Expect(TokenKind::LeftParen, "expected '(' after switch")) {
    return nullptr;
  }
  statement->condition = ParseExpression();
  if (!statement->condition || !Expect(TokenKind::RightParen, "expected ')' after switch value") ||
      !Expect(TokenKind::LeftBrace, "expected '{' before switch cases")) {
    return nullptr;
  }
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    Statement::Case branch;
    branch.location = Peek().location;
    if (Match(TokenKind::Case)) {
      branch.label = ParseExpression();
      if (!branch.label) {
        return nullptr;
      }
    } else if (!Match(TokenKind::Default)) {
      AddDiagnostic(Peek().location, "expected 'case' or 'default' in switch");
      return nullptr;
    }
    if (!Expect(TokenKind::Colon, "expected ':' after switch label")) {
      return nullptr;
    }
    branch.body = std::make_unique<Statement>();
    branch.body->kind = Statement::Kind::Block;
    branch.body->location = branch.location;
    while (!Check(TokenKind::Case) && !Check(TokenKind::Default) && !Check(TokenKind::RightBrace) &&
           !IsAtEnd()) {
      auto child = ParseStatement();
      if (!child) {
        return nullptr;
      }
      branch.body->statements.push_back(std::move(child));
    }
    statement->cases.push_back(std::move(branch));
  }
  if (!Expect(TokenKind::RightBrace, "expected '}' after switch cases")) {
    return nullptr;
  }
  return statement;
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
  const Token keyword = tokens_.subspan(current_++).front();
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
  // 已解析的表达式可保留为完整 AST 节点；缺分号仍产生诊断，编译入口不会接纳它。
  // 不在此同步，否则会吞掉光标下一行的完整语句，破坏编辑器的符号绑定。
  static_cast<void>(Expect(TokenKind::Semicolon, "expected ';' after expression"));
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
  while (Check(TokenKind::Is) || Check(TokenKind::As)) {
    const auto operation = tokens_.subspan(current_++).front();
    auto type = ParseType();
    if (!type || !expression) {
      return nullptr;
    }
    auto typed = MakeExpression(
        operation.kind == TokenKind::Is ? Expression::Kind::TypeTest : Expression::Kind::TypeCast,
        operation.location);
    typed->operand = std::move(expression);
    typed->type = std::move(*type);
    expression = std::move(typed);
  }
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
    AnnotatePrevious(TokenRole::PrefixOperator);
    const Token operator_token = Previous();
    auto unary = MakeExpression(Expression::Kind::Unary, operator_token.location);
    unary->text = operator_token.lexeme;
    unary->operand = ParseUnary();
    if (operator_token.kind == TokenKind::Minus && unary->operand &&
        unary->operand->kind == Expression::Kind::IntegerLiteral &&
        !unary->operand->text.starts_with('-')) {
      unary->operand->text.insert(unary->operand->text.begin(), '-');
      unary->operand->location = operator_token.location;
      return std::move(unary->operand);
    }
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
    if ((expression->kind == Expression::Kind::Name ||
         expression->kind == Expression::Kind::Member) &&
        IsTypeApplication()) {
      if (!expression->type_arguments.empty()) {
        AddDiagnostic(Peek().location, "type arguments are already supplied");
        return nullptr;
      }
      expression->type_arguments = ParseTypeArguments();
    } else if (Match(TokenKind::LeftParen)) {
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
  if (Match(TokenKind::Match)) {
    return ParseMatch();
  }
  if (Match(TokenKind::Null)) {
    return MakeExpression(Expression::Kind::NullLiteral, Previous().location);
  }
  if (Match(TokenKind::IntegerLiteral)) {
    auto expression = MakeExpression(Expression::Kind::IntegerLiteral, Previous().location);
    expression->text = Previous().lexeme;
    return expression;
  }
  if (Match(TokenKind::DoubleLiteral)) {
    return ParseDoubleLiteral();
  }
  if (Check(TokenKind::StringLiteral) || Check(TokenKind::StringHead)) {
    return ParseString();
  }
  if (Match(TokenKind::LeftBracket)) {
    return ParseArrayLiteral();
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
  if (Match(TokenKind::LeftParen)) {
    if (Check(TokenKind::RightParen) ||
        (Check(TokenKind::Identifier) &&
         tokens_.subspan(current_ + 1).front().kind == TokenKind::Colon)) {
      return ParseLambda();
    }
    std::unique_ptr<Expression> expression = ParseExpression();
    if (!Expect(TokenKind::RightParen, "expected ')' after expression").has_value()) {
      return nullptr;
    }
    return expression;
  }

  AddDiagnostic(Peek().location, "expected expression");
  return nullptr;
}

auto Parser::ParseArrayLiteral() -> std::unique_ptr<Expression> {
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
  if (!Expect(TokenKind::RightBracket, "expected ']' after array literal")) {
    return nullptr;
  }
  return expression;
}

auto Parser::ParseMatch() -> std::unique_ptr<Expression> {
  auto expression = MakeExpression(Expression::Kind::Match, Previous().location);
  if (!Expect(TokenKind::LeftParen, "expected '(' after match")) {
    return nullptr;
  }
  expression->operand = ParseExpression();
  if (!expression->operand || !Expect(TokenKind::RightParen, "expected ')' after match selector") ||
      !Expect(TokenKind::LeftBrace, "expected '{' before match arms")) {
    return nullptr;
  }
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    MatchArm arm;
    arm.location = Peek().location;
    if (Match(TokenKind::Let)) {
      const auto name = Expect(TokenKind::Identifier, "expected pattern binding name");
      if (!name) {
        return nullptr;
      }
      arm.kind = MatchArm::Kind::Binding;
      arm.binding = name->lexeme;
    } else if (Check(TokenKind::Identifier) && Peek().lexeme == "_") {
      ++current_;
      arm.kind = MatchArm::Kind::Wildcard;
    } else {
      arm.pattern = ParseExpression();
      if (!arm.pattern) {
        return nullptr;
      }
    }
    if (!Expect(TokenKind::Arrow, "expected '=>' after match pattern")) {
      return nullptr;
    }
    arm.value = ParseExpression();
    if (!arm.value) {
      return nullptr;
    }
    expression->arms.push_back(std::move(arm));
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (!Expect(TokenKind::RightBrace, "expected '}' after match arms")) {
    return nullptr;
  }
  return expression;
}

auto Parser::ParseLambda() -> std::unique_ptr<Expression> {
  auto expression = MakeExpression(Expression::Kind::Lambda, Previous().location);
  auto function = std::make_unique<Function>();
  function->location = expression->location;
  while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
    auto parameter = ParseParameter();
    if (!parameter) {
      return nullptr;
    }
    function->parameters.push_back(std::move(*parameter));
    if (!Match(TokenKind::Comma)) {
      break;
    }
  }
  if (!Expect(TokenKind::RightParen, "expected ')' after lambda parameters") ||
      !Expect(TokenKind::Colon, "expected ':' before lambda return type")) {
    return nullptr;
  }
  auto returned = ParseType();
  if (!returned || !Expect(TokenKind::Arrow, "expected '=>' before lambda body")) {
    return nullptr;
  }
  function->return_type = std::move(*returned);
  if (Check(TokenKind::LeftBrace)) {
    function->body = ParseBlock();
  } else {
    auto value = ParseExpression();
    if (!value) {
      return nullptr;
    }
    auto statement = std::make_unique<Statement>();
    statement->kind = Statement::Kind::Return;
    statement->location = value->location;
    statement->expression = std::move(value);
    function->body = std::make_unique<Statement>();
    function->body->kind = Statement::Kind::Block;
    function->body->location = expression->location;
    function->body->statements.push_back(std::move(statement));
  }
  if (!function->body) {
    return nullptr;
  }
  expression->function = std::move(function);
  return expression;
}

auto Parser::ParseString() -> std::unique_ptr<Expression> {
  if (Match(TokenKind::StringLiteral)) {
    auto literal = MakeExpression(Expression::Kind::StringLiteral, Previous().location);
    literal->text = Previous().lexeme;
    return literal;
  }
  auto interpolation = MakeExpression(Expression::Kind::StringInterpolation, Peek().location);
  ++current_;
  while (true) {
    auto literal = MakeExpression(Expression::Kind::StringLiteral, Previous().location);
    literal->text = Previous().lexeme;
    interpolation->elements.push_back(std::move(literal));
    auto expression = ParseExpression();
    if (!expression) {
      return nullptr;
    }
    interpolation->elements.push_back(std::move(expression));
    if (Match(TokenKind::StringTail)) {
      auto tail = MakeExpression(Expression::Kind::StringLiteral, Previous().location);
      tail->text = Previous().lexeme;
      interpolation->elements.push_back(std::move(tail));
      return interpolation;
    }
    if (!Expect(TokenKind::StringMiddle, "expected '}' after interpolation expression")) {
      return nullptr;
    }
  }
}

auto Parser::ParseArguments() -> std::vector<CallArgument> {
  std::vector<CallArgument> arguments;
  while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
    CallArgument argument;
    argument.location = Peek().location;
    if (Check(TokenKind::Identifier) && current_ + 1 < tokens_.size() &&
        tokens_.subspan(current_ + 1).front().kind == TokenKind::Colon) {
      argument.name = tokens_.subspan(current_++).front().lexeme;
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
