#include "compiler/frontend/syntax/parser.h"

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
#include "base/source_lines.h"
#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/lexer.h"
#include "compiler/frontend/syntax/token.h"

namespace vela::frontend {
namespace {

std::optional<TokenKind> CompoundOperator(TokenKind operation) {
  switch (operation) {
    case TokenKind::PlusEqual:
      return TokenKind::Plus;
    case TokenKind::MinusEqual:
      return TokenKind::Minus;
    case TokenKind::StarEqual:
      return TokenKind::Star;
    case TokenKind::SlashEqual:
      return TokenKind::Slash;
    case TokenKind::PercentEqual:
      return TokenKind::Percent;
    case TokenKind::AmpersandEqual:
      return TokenKind::Ampersand;
    case TokenKind::PipeEqual:
      return TokenKind::Pipe;
    case TokenKind::CaretEqual:
      return TokenKind::Caret;
    case TokenKind::ShiftLeftEqual:
      return TokenKind::ShiftLeft;
    case TokenKind::ShiftRightEqual:
      return TokenKind::ShiftRight;
    case TokenKind::ShiftUnsignedRightEqual:
      return TokenKind::ShiftUnsignedRight;
    default:
      return std::nullopt;
  }
}

}  // namespace

ParseResult ParseSource(std::string_view source, std::uint32_t source_id) {
  auto lexed = Lexer(source, source_id).Tokenize();
  if (!lexed.diagnostics.empty()) {
    return {.program = Program(), .diagnostics = std::move(lexed.diagnostics)};
  }
  return Parser(lexed.tokens).Parse();
}

ParsedSource ParseSourceSnapshot(std::string source,
                                 std::uint32_t source_id,
                                 SyntaxContext context) {
  auto lexed = Lexer(source, source_id, CommentRetention::Preserve).Tokenize();
  auto parsed = lexed.diagnostics.empty()
                    ? Parser(lexed.tokens, context).Parse()
                    : ParseResult{.program = {}, .diagnostics = std::move(lexed.diagnostics)};
  auto lines = IndexSourceLines(source);
  auto standard_names = CollectStandardNames(lexed.tokens);
  auto snapshot = std::make_shared<const SourceSnapshot>(SourceSnapshot{
      .text = std::move(source),
      .lines = std::move(lines),
      .tokens = std::move(lexed.tokens),
      .comments = std::move(lexed.comments),
      .imports = parsed.program.imports,
      .standard_names = std::move(standard_names),
  });
  return {
      .source = std::move(snapshot),
      .program = std::move(parsed.program),
      .diagnostics = std::move(parsed.diagnostics),
  };
}

void AppendProgram(Program& destination, Program source) {
  destination.origins.merge(source.origins);
  std::ranges::move(source.enums, std::back_inserter(destination.enums));
  std::ranges::move(source.classes, std::back_inserter(destination.classes));
  std::ranges::move(source.functions, std::back_inserter(destination.functions));
  std::ranges::move(source.imports, std::back_inserter(destination.imports));
  std::ranges::move(source.aliases, std::back_inserter(destination.aliases));
  std::ranges::move(source.interfaces, std::back_inserter(destination.interfaces));
  std::ranges::move(source.globals, std::back_inserter(destination.globals));
  std::ranges::move(source.standard_names, std::back_inserter(destination.standard_names));
}

ParseResult Parser::Parse() {
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
  switch (kind) {
    case TokenKind::External:
      if (current_ + 1 < tokens_.size() && tokens_[current_ + 1].kind == TokenKind::Class) {
        Advance();
        Advance();
        append(ParseClass(ClassKind::Native), program.classes);
      } else {
        append(ParseFunction(), program.functions);
      }
      break;
    case TokenKind::Async:
      append(ParseFunction(), program.functions);
      break;
    case TokenKind::Interface:
      Advance();
      append(ParseInterface(), program.interfaces);
      break;
    case TokenKind::Type:
      Advance();
      append(ParseTypeAlias(), program.aliases);
      break;
    case TokenKind::Import:
      Advance();
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
      Advance();
      append(ParseEnum(), program.enums);
      break;
    case TokenKind::Class:
      Advance();
      append(ParseClass(), program.classes);
      break;
    case TokenKind::Let:
    case TokenKind::Const: {
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
      append(ParseFunction(), program.functions);
      break;
  }
}

std::optional<Program::Import> Parser::ParseImport() {
  if (!Match(TokenKind::StringLiteral)) {
    AddDiagnostic(Peek().location, "expected string path after import");
    return std::nullopt;
  }
  const auto& path = Previous();
  std::string alias;
  if (Match(TokenKind::As)) {
    const auto* const name = Expect(TokenKind::Identifier, "expected import prefix after 'as'");
    if (name == nullptr) {
      return std::nullopt;
    }
    alias = name->lexeme;
  }
  if (Expect(TokenKind::Semicolon, "expected ';' after import") == nullptr) {
    return std::nullopt;
  }
  if (path.lexeme.contains('\0')) {
    AddDiagnostic(path.location, "import path cannot contain a null byte");
    return std::nullopt;
  }
  return Program::Import{.path = path.lexeme, .location = path.location, .alias = std::move(alias)};
}

void Parser::AnnotatePrevious(TokenRole role) {
  tokens_[previous_index_].role = role;
}

const Token& Parser::Peek() const {
  return remainder_ ? *remainder_ : tokens_[current_];
}

const Token& Parser::Previous() const {
  return previous_split_ ? *previous_split_ : tokens_[previous_index_];
}

const Token& Parser::Advance() {
  previous_index_ = current_++;
  previous_split_ = std::move(remainder_);
  remainder_.reset();
  return Previous();
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
  Advance();
  return true;
}

const Token* Parser::TakeName() {
  const auto kind = Peek().kind;
  if (IsDeclarationName(kind)) {
    return &Advance();
  }
  return nullptr;
}

const Token* Parser::Expect(TokenKind kind, std::string message) {
  if (Check(kind)) {
    return &Advance();
  }
  AddDiagnostic(Peek().location, std::move(message));
  return nullptr;
}

void Parser::AddDiagnostic(SourceLocation location, std::string message) {
  diagnostics_.push_back({.location = location, .message = std::move(message), .source_path = {}});
}

void Parser::Synchronize() {
  if (IsDeclarationStart(Peek().kind) && !IsVariableDeclaration()) {
    return;
  }
  if (!IsAtEnd()) {
    Advance();
  }
  while (!IsAtEnd()) {
    if (Previous().kind == TokenKind::Semicolon || IsDeclarationStart(Peek().kind) ||
        Check(TokenKind::If) || Check(TokenKind::Return) || Check(TokenKind::While)) {
      return;
    }
    Advance();
  }
}

std::unique_ptr<Statement> Parser::ParseBlock(BlockKind kind) {
  const auto* left_brace = Expect(TokenKind::LeftBrace, "expected '{'");
  if (left_brace == nullptr) {
    return nullptr;
  }
  auto block = std::make_unique<Statement>();
  block->kind = Statement::Kind::Block;
  block->location = left_brace->location;
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    if (IsDeclarationStart(Peek().kind) && !IsVariableDeclaration()) {
      AddDiagnostic(Peek().location, "expected '}' before declaration");
      return nullptr;
    }
    const auto start = current_;
    std::unique_ptr<Statement> statement = ParseStatement();
    if (kind == BlockKind::Lambda && block->statements.empty() && statement &&
        statement->kind == Statement::Kind::Expression && Check(TokenKind::Colon)) {
      AddDiagnostic(statement->location,
                    "map literal in a lambda expression body requires parentheses; "
                    "use => ({'key': value})");
    }
    if (statement) {
      block->statements.push_back(std::move(statement));
    } else if (current_ == start || !Check(TokenKind::RightBrace)) {
      Synchronize();
    }
  }
  static_cast<void>(Expect(TokenKind::RightBrace, "expected '}' after block"));
  return block;
}

bool Parser::IsVariableDeclaration() const {
  return Check(TokenKind::Let) || Check(TokenKind::Const);
}

std::unique_ptr<Statement> Parser::ParseStatement() {
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

std::unique_ptr<Statement> Parser::ParseForStatement() {
  const auto location = Previous().location;
  if (Expect(TokenKind::LeftParen, "expected '(' after for") == nullptr) {
    return nullptr;
  }
  if (Match(TokenKind::Const)) {
    return ParseForEachStatement();
  }
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::For;
  statement->location = location;
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
    Advance();
  }
  if (!Check(TokenKind::Semicolon)) {
    statement->condition = ParseExpression();
    if (!statement->condition) {
      return nullptr;
    }
  }
  if (Expect(TokenKind::Semicolon, "expected ';' after for condition") == nullptr) {
    return nullptr;
  }
  if (!Check(TokenKind::RightParen)) {
    statement->increment = ParseExpression();
    if (!statement->increment) {
      return nullptr;
    }
  }
  if (Expect(TokenKind::RightParen, "expected ')' after for clauses") == nullptr) {
    return nullptr;
  }
  statement->then_branch = ParseStatement();
  return statement->then_branch ? std::move(statement) : nullptr;
}

std::unique_ptr<Statement> Parser::ParseForEachStatement() {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::ForEach;
  statement->location = Previous().location;
  statement->binding = BindingKind::Constant;
  const auto* const name = TakeName();
  if (name == nullptr) {
    AddDiagnostic(Peek().location, "expected name after 'const' in collection loop");
    return nullptr;
  }
  statement->name = name->lexeme;
  if (Expect(TokenKind::In, "expected 'in' in collection loop") == nullptr) {
    return nullptr;
  }
  statement->iterable = ParseExpression();
  if (!statement->iterable ||
      (Expect(TokenKind::RightParen, "expected ')' after collection loop") == nullptr)) {
    return nullptr;
  }
  statement->then_branch = ParseStatement();
  return statement->then_branch ? std::move(statement) : nullptr;
}

std::unique_ptr<Statement> Parser::ParseSwitchStatement() {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::Switch;
  statement->location = Previous().location;
  if (Expect(TokenKind::LeftParen, "expected '(' after switch") == nullptr) {
    return nullptr;
  }
  statement->condition = ParseExpression();
  if (!statement->condition ||
      (Expect(TokenKind::RightParen, "expected ')' after switch value") == nullptr) ||
      (Expect(TokenKind::LeftBrace, "expected '{' before switch cases") == nullptr)) {
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
    if (Expect(TokenKind::Colon, "expected ':' after switch label") == nullptr) {
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
  if (Expect(TokenKind::RightBrace, "expected '}' after switch cases") == nullptr) {
    return nullptr;
  }
  return statement;
}

std::unique_ptr<Statement> Parser::ParseBreakStatement() {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::Break;
  statement->location = Previous().location;
  if (Expect(TokenKind::Semicolon, "expected ';' after break") == nullptr) {
    return nullptr;
  }
  return statement;
}

std::unique_ptr<Statement> Parser::ParseContinueStatement() {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::Continue;
  statement->location = Previous().location;
  if (Expect(TokenKind::Semicolon, "expected ';' after continue") == nullptr) {
    return nullptr;
  }
  return statement;
}

std::unique_ptr<Statement> Parser::ParseIfStatement() {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::If;
  statement->location = Previous().location;
  if (Expect(TokenKind::LeftParen, "expected '(' after if") == nullptr) {
    return nullptr;
  }
  statement->condition = ParseExpression();
  if (!statement->condition ||
      (Expect(TokenKind::RightParen, "expected ')' after if condition") == nullptr)) {
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

std::unique_ptr<Statement> Parser::ParseWhileStatement() {
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::While;
  statement->location = Previous().location;
  if (Expect(TokenKind::LeftParen, "expected '(' after while") == nullptr) {
    return nullptr;
  }
  statement->condition = ParseExpression();
  if (!statement->condition ||
      (Expect(TokenKind::RightParen, "expected ')' after while condition") == nullptr)) {
    return nullptr;
  }
  statement->then_branch = ParseStatement();
  return statement->then_branch ? std::move(statement) : nullptr;
}

std::unique_ptr<Statement> Parser::ParseVariableDeclaration() {
  const auto binding =
      Advance().kind == TokenKind::Const ? BindingKind::Constant : BindingKind::Mutable;
  const auto* const name = TakeName();
  if (name == nullptr) {
    return nullptr;
  }
  auto statement = std::make_unique<Statement>();
  statement->kind = Statement::Kind::VariableDeclaration;
  statement->location = name->location;
  statement->name = name->lexeme;
  statement->binding = binding;
  if (Match(TokenKind::Colon)) {
    auto type = ParseType();
    if (!type) {
      return nullptr;
    }
    statement->type = std::move(*type);
  }
  if (Expect(TokenKind::Equal, "variable declarations require an initializer") == nullptr) {
    return nullptr;
  }
  statement->expression = ParseExpression();
  if (!statement->expression) {
    return nullptr;
  }
  static_cast<void>(Expect(TokenKind::Semicolon, "expected ';' after variable declaration"));
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
  static_cast<void>(Expect(TokenKind::Semicolon, "expected ';' after return statement"));
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
  // 已解析的表达式可保留为完整 AST 节点；缺分号仍产生诊断，编译入口不会接纳它。
  // 不在此同步，否则会吞掉光标下一行的完整语句，破坏编辑器的符号绑定。
  static_cast<void>(Expect(TokenKind::Semicolon, "expected ';' after expression"));
  return statement;
}

std::unique_ptr<Expression> Parser::ParseExpression() {
  return ParseAssignment();
}

std::unique_ptr<Expression> Parser::ParseAssignment() {
  std::unique_ptr<Expression> expression = ParseConditional();
  const auto compound = CompoundOperator(Peek().kind);
  if (Check(TokenKind::Equal) || compound) {
    const auto operation = Advance().kind;
    const SourceLocation location = Previous().location;
    std::unique_ptr<Expression> right = ParseAssignment();
    if (!right) {
      return nullptr;
    }
    auto assignment = MakeExpression(
        compound ? Expression::Kind::CompoundAssignment : Expression::Kind::Binary, location);
    assignment->operation = compound ? *compound : operation;
    assignment->left = std::move(expression);
    assignment->right = std::move(right);
    return assignment;
  }
  return expression;
}

std::unique_ptr<Expression> Parser::ParseConditional() {
  auto expression = ParseBinary();
  if (!Match(TokenKind::Question)) {
    return expression;
  }
  AnnotatePrevious(TokenRole::ConditionalOperator);
  const auto location = Previous().location;
  auto when_true = ParseExpression();
  if (!when_true ||
      (Expect(TokenKind::Colon, "expected ':' in conditional expression") == nullptr)) {
    return nullptr;
  }
  AnnotatePrevious(TokenRole::ConditionalOperator);
  auto when_false = ParseConditional();
  if (!when_false) {
    return nullptr;
  }
  auto conditional = MakeExpression(Expression::Kind::Conditional, location);
  conditional->operand = std::move(expression);
  conditional->left = std::move(when_true);
  conditional->right = std::move(when_false);
  return conditional;
}

namespace {

constexpr std::uint8_t kEqualityPrecedence = 6;
constexpr std::uint8_t kShiftPrecedence = 8;
constexpr std::uint8_t kBitAndPrecedence = 5;
constexpr std::uint8_t kAdditivePrecedence = 9;
constexpr std::uint8_t kMultiplicativePrecedence = 10;

std::uint8_t BinaryPrecedence(TokenKind kind) {
  switch (kind) {
    case TokenKind::OrOr:
    case TokenKind::QuestionQuestion:
      return 1;
    case TokenKind::AndAnd:
      return 2;
    case TokenKind::Pipe:
      return 3;
    case TokenKind::Caret:
      return 4;
    case TokenKind::Ampersand:
      return kBitAndPrecedence;
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual:
      return kEqualityPrecedence;
    case TokenKind::ShiftLeft:
    case TokenKind::ShiftRight:
    case TokenKind::ShiftUnsignedRight:
      return kShiftPrecedence;
    case TokenKind::Plus:
    case TokenKind::Minus:
      return kAdditivePrecedence;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
      return kMultiplicativePrecedence;
    default:
      return 0;
  }
}

}  // namespace

std::unique_ptr<Expression> Parser::ParseBinary(std::uint8_t minimum_precedence) {
  // is/as 只出现在关系式的首个移位表达式后；保留这一语法边界，
  // 避免把 a < b is T 或 a is T + b 接纳为新的表达式形式。
  const bool below_relational = minimum_precedence < kShiftPrecedence;
  auto expression = below_relational ? ParseRelational() : ParseUnary();
  if (!expression) {
    return nullptr;
  }
  while (true) {
    const auto precedence = BinaryPrecedence(Peek().kind);
    if (precedence < minimum_precedence || (below_relational && precedence > kEqualityPrecedence)) {
      return expression;
    }
    const auto operation = Advance().kind;
    const auto location = Previous().location;
    auto right = ParseBinary(precedence + 1);
    if (!right) {
      return nullptr;
    }
    auto binary = MakeExpression(Expression::Kind::Binary, location);
    binary->operation = operation;
    binary->left = std::move(expression);
    binary->right = std::move(right);
    expression = std::move(binary);
  }
}

std::unique_ptr<Expression> Parser::ParseRelational() {
  std::unique_ptr<Expression> expression = ParseBinary(kShiftPrecedence);
  while (Check(TokenKind::Is) || Check(TokenKind::As)) {
    const auto operation = Advance().kind;
    const auto location = Previous().location;
    auto type = ParseType();
    if (!type || !expression) {
      return nullptr;
    }
    auto typed = MakeExpression(
        operation == TokenKind::Is ? Expression::Kind::TypeTest : Expression::Kind::TypeCast,
        location);
    typed->operand = std::move(expression);
    typed->type = std::move(*type);
    expression = std::move(typed);
  }
  while (Match(TokenKind::Less) || Match(TokenKind::LessEqual) || Match(TokenKind::Greater) ||
         Match(TokenKind::GreaterEqual)) {
    const auto operation = Previous().kind;
    const auto location = Previous().location;
    auto right = ParseBinary(kShiftPrecedence);
    if (!right) {
      return nullptr;
    }
    auto binary = MakeExpression(Expression::Kind::Binary, location);
    binary->operation = operation;
    binary->left = std::move(expression);
    binary->right = std::move(right);
    expression = std::move(binary);
  }
  return expression;
}

std::unique_ptr<Expression> Parser::ParseUnary() {
  if (Match(TokenKind::Await)) {
    auto await = MakeExpression(Expression::Kind::Await, Previous().location);
    await->operand = ParseUnary();
    return await->operand ? std::move(await) : nullptr;
  }
  if (Match(TokenKind::Bang) || Match(TokenKind::Minus) || Match(TokenKind::Tilde) ||
      Match(TokenKind::PlusPlus) || Match(TokenKind::MinusMinus)) {
    AnnotatePrevious(TokenRole::PrefixOperator);
    auto unary = MakeExpression(Expression::Kind::Unary, Previous().location);
    unary->operation = Previous().kind;
    unary->operand = ParseUnary();
    if (unary->operation == TokenKind::Minus && unary->operand &&
        unary->operand->kind == Expression::Kind::IntegerLiteral &&
        !unary->operand->text.starts_with('-')) {
      unary->operand->text.insert(unary->operand->text.begin(), '-');
      unary->operand->location = unary->location;
      return std::move(unary->operand);
    }
    return unary->operand ? std::move(unary) : nullptr;
  }
  return ParsePostfix();
}

std::unique_ptr<Expression> Parser::ParsePostfix() {
  auto expression = ParsePrimary();
  if (!expression) {
    return nullptr;
  }
  while (true) {
    auto status = ParsePostfixTypeApplication(expression);
    if (status == PostfixStatus::Unmatched) {
      status = ParsePostfixCall(expression);
    }
    if (status == PostfixStatus::Unmatched) {
      status = ParsePostfixMember(expression);
    }
    if (status == PostfixStatus::Unmatched) {
      status = ParsePostfixIndex(expression);
    }
    if (status == PostfixStatus::Unmatched) {
      status = ParsePostfixIncrement(expression);
    }
    switch (status) {
      case PostfixStatus::Unmatched:
        return expression;
      case PostfixStatus::Failed:
        return nullptr;
      case PostfixStatus::Parsed:
        break;
    }
  }
}

Parser::PostfixStatus Parser::ParsePostfixTypeApplication(std::unique_ptr<Expression>& expression) {
  if ((expression->kind != Expression::Kind::Name &&
       expression->kind != Expression::Kind::Member) ||
      !IsTypeApplication()) {
    return PostfixStatus::Unmatched;
  }
  if (!expression->type_arguments.empty()) {
    AddDiagnostic(Peek().location, "type arguments are already supplied");
    expression.reset();
    return PostfixStatus::Failed;
  }
  expression->type_arguments = ParseTypeArguments();
  return PostfixStatus::Parsed;
}

Parser::PostfixStatus Parser::ParsePostfixCall(std::unique_ptr<Expression>& expression) {
  if (!Match(TokenKind::LeftParen)) {
    return PostfixStatus::Unmatched;
  }
  auto call = MakeExpression(Expression::Kind::Call, expression->location);
  call->callee = std::move(expression);
  call->arguments = ParseArguments();
  if (Expect(TokenKind::RightParen, "expected ')' after arguments") == nullptr) {
    expression.reset();
    return PostfixStatus::Failed;
  }
  expression = std::move(call);
  return PostfixStatus::Parsed;
}

Parser::PostfixStatus Parser::ParsePostfixMember(std::unique_ptr<Expression>& expression) {
  const bool null_safe = Match(TokenKind::QuestionDot);
  const bool ordinary = !null_safe && Match(TokenKind::Dot);
  if (!null_safe && !ordinary) {
    return PostfixStatus::Unmatched;
  }
  if (null_safe && Match(TokenKind::LeftBracket)) {
    auto index = MakeExpression(Expression::Kind::Index, Previous().location);
    index->object = std::move(expression);
    index->index = ParseExpression();
    if (!index->index || (Expect(TokenKind::RightBracket, "expected ']' after '?.['") == nullptr)) {
      expression.reset();
      return PostfixStatus::Failed;
    }
    index->null_safe = true;
    expression = std::move(index);
    return PostfixStatus::Parsed;
  }
  std::optional<Token> member;
  if (IsMemberName(Peek().kind)) {
    member = Advance();
  } else {
    AddDiagnostic(Peek().location,
                  null_safe ? "expected member name after '?.'" : "expected member name after '.'");
  }
  if (!member) {
    expression.reset();
    return PostfixStatus::Failed;
  }
  auto access = MakeExpression(Expression::Kind::Member, member->location);
  access->object = std::move(expression);
  access->text = member->lexeme;
  access->null_safe = null_safe;
  expression = std::move(access);
  return PostfixStatus::Parsed;
}

Parser::PostfixStatus Parser::ParsePostfixIndex(std::unique_ptr<Expression>& expression) {
  if (!Match(TokenKind::LeftBracket)) {
    return PostfixStatus::Unmatched;
  }
  auto index = MakeExpression(Expression::Kind::Index, Previous().location);
  index->object = std::move(expression);
  index->index = ParseExpression();
  if (!index->index || (Expect(TokenKind::RightBracket, "expected ']' after index") == nullptr)) {
    expression.reset();
    return PostfixStatus::Failed;
  }
  expression = std::move(index);
  return PostfixStatus::Parsed;
}

Parser::PostfixStatus Parser::ParsePostfixIncrement(std::unique_ptr<Expression>& expression) {
  if (!Match(TokenKind::PlusPlus) && !Match(TokenKind::MinusMinus)) {
    return PostfixStatus::Unmatched;
  }
  const auto& token = Previous();
  auto update = MakeExpression(Expression::Kind::Unary, token.location);
  update->operation = token.kind;
  update->postfix = true;
  update->operand = std::move(expression);
  expression = std::move(update);
  return PostfixStatus::Parsed;
}

std::unique_ptr<Expression> Parser::ParseDoubleLiteral() {
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

bool Parser::IsLambda() const {
  if (Check(TokenKind::RightParen)) {
    return true;
  }
  if (!Check(TokenKind::Identifier)) {
    return false;
  }
  const auto next = tokens_[current_ + 1].kind;
  if (next == TokenKind::Colon || next == TokenKind::Comma) {
    return true;
  }
  if (next != TokenKind::RightParen) {
    return false;
  }
  const auto after = tokens_[current_ + 2].kind;
  return after == TokenKind::Arrow || after == TokenKind::Colon;
}

std::unique_ptr<Expression> Parser::ParsePrimary() {
  if (Match(TokenKind::Async)) {
    auto expression = ParsePrimary();
    if (!expression || expression->kind != Expression::Kind::Lambda || !expression->function) {
      AddDiagnostic(Peek().location, "expected async lambda expression");
      return nullptr;
    }
    expression->function->is_async = true;
    return expression;
  }
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
  if (Match(TokenKind::LeftBrace)) {
    return ParseMapLiteral();
  }
  if (Match(TokenKind::True) || Match(TokenKind::False)) {
    auto expression = MakeExpression(Expression::Kind::BooleanLiteral, Previous().location);
    expression->boolean_value = Previous().kind == TokenKind::True;
    return expression;
  }
  if (Match(TokenKind::Identifier) || Match(TokenKind::Get) || Match(TokenKind::Set) ||
      Match(TokenKind::Print) || Match(TokenKind::Super)) {
    if (Check(TokenKind::Arrow) && Previous().kind == TokenKind::Identifier) {
      return ParseLambda(LambdaParameters::Single);
    }
    auto expression = MakeExpression(Expression::Kind::Name, Previous().location);
    expression->text = Previous().lexeme;
    return expression;
  }
  if (Match(TokenKind::LeftParen)) {
    if (IsLambda()) {
      return ParseLambda(LambdaParameters::Parenthesized);
    }
    std::unique_ptr<Expression> expression = ParseExpression();
    if (Expect(TokenKind::RightParen, "expected ')' after expression") == nullptr) {
      return nullptr;
    }
    return expression;
  }

  AddDiagnostic(Peek().location, "expected expression");
  return nullptr;
}

std::unique_ptr<Expression> Parser::ParseMatch() {
  auto expression = MakeExpression(Expression::Kind::Match, Previous().location);
  if (Expect(TokenKind::LeftParen, "expected '(' after match") == nullptr) {
    return nullptr;
  }
  expression->operand = ParseExpression();
  if (!expression->operand ||
      (Expect(TokenKind::RightParen, "expected ')' after match selector") == nullptr) ||
      (Expect(TokenKind::LeftBrace, "expected '{' before match arms") == nullptr)) {
    return nullptr;
  }
  while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
    MatchArm arm;
    arm.location = Peek().location;
    if (Match(TokenKind::Let)) {
      const auto* const name = Expect(TokenKind::Identifier, "expected pattern binding name");
      if (name == nullptr) {
        return nullptr;
      }
      arm.kind = MatchArm::Kind::Binding;
      arm.binding = name->lexeme;
    } else if (Check(TokenKind::Identifier) && Peek().lexeme == "_") {
      Advance();
      arm.kind = MatchArm::Kind::Wildcard;
    } else {
      arm.pattern = ParseExpression();
      if (!arm.pattern) {
        return nullptr;
      }
    }
    if (Expect(TokenKind::Arrow, "expected '=>' after match pattern") == nullptr) {
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
  if (Expect(TokenKind::RightBrace, "expected '}' after match arms") == nullptr) {
    return nullptr;
  }
  return expression;
}

std::unique_ptr<Expression> Parser::ParseLambda(LambdaParameters parameters) {
  auto expression = MakeExpression(Expression::Kind::Lambda, Previous().location);
  auto function = std::make_unique<Function>();
  function->location = expression->location;
  if (parameters == LambdaParameters::Single) {
    const auto& name = Previous();
    function->parameters.push_back({
        .type = {.name = {}, .location = name.location, .arguments = {}, .parameter = {}},
        .name = name.lexeme,
        .location = name.location,
    });
  } else {
    auto parsed_parameters = ParseParameters(ParameterTyping::Inferred);
    if (!parsed_parameters) {
      return nullptr;
    }
    function->parameters = std::move(*parsed_parameters);
    if (Expect(TokenKind::RightParen, "expected ')' after lambda parameters") == nullptr) {
      return nullptr;
    }
  }
  if (Match(TokenKind::Colon)) {
    auto returned = ParseType();
    if (!returned) {
      return nullptr;
    }
    function->return_type = std::move(*returned);
  }
  if (Expect(TokenKind::Arrow, "expected '=>' before lambda body") == nullptr) {
    return nullptr;
  }
  if (Check(TokenKind::LeftBrace)) {
    function->body = ParseBlock(BlockKind::Lambda);
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

std::unique_ptr<Expression> Parser::ParseString() {
  if (Match(TokenKind::StringLiteral)) {
    auto literal = MakeExpression(Expression::Kind::StringLiteral, Previous().location);
    literal->text = Previous().lexeme;
    return literal;
  }
  auto interpolation = MakeExpression(Expression::Kind::StringInterpolation, Peek().location);
  Advance();
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
    if (Expect(TokenKind::StringMiddle, "expected '}' after interpolation expression") == nullptr) {
      return nullptr;
    }
  }
}

std::vector<CallArgument> Parser::ParseArguments() {
  std::vector<CallArgument> arguments;
  while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
    CallArgument argument;
    argument.location = Peek().location;
    if (Check(TokenKind::Identifier) && current_ + 1 < tokens_.size() &&
        tokens_[current_ + 1].kind == TokenKind::Colon) {
      argument.name = Advance().lexeme;
      Advance();
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
