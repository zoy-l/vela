#include "compiler/frontend/lexer.h"

#include <cctype>
#include <unordered_map>

namespace vela::frontend {
namespace {

const std::unordered_map<std::string_view, TokenKind> kKeywords = {
    {"class", TokenKind::Class},   {"else", TokenKind::Else},   {"enum", TokenKind::Enum},
    {"false", TokenKind::False},   {"if", TokenKind::If},       {"return", TokenKind::Return},
    {"true", TokenKind::True},     {"void", TokenKind::Void},   {"while", TokenKind::While},
    {"import", TokenKind::Import}, {"break", TokenKind::Break}, {"continue", TokenKind::Continue},
    {"for", TokenKind::For},       {"print", TokenKind::Print},
};

auto IsIdentifierStart(char character) -> bool {
  const auto value = static_cast<unsigned char>(character);
  return std::isalpha(value) != 0 || character == '_';
}

auto IsIdentifierPart(char character) -> bool {
  const auto value = static_cast<unsigned char>(character);
  return std::isalnum(value) != 0 || character == '_';
}

auto IsDigit(char character) -> bool {
  const auto value = static_cast<unsigned char>(character);
  return std::isdigit(value) != 0;
}

}  // namespace

auto Lexer::Tokenize() -> LexResult {
  while (!IsAtEnd()) {
    SkipWhitespaceAndComments();
    if (IsAtEnd()) {
      break;
    }

    token_location_ = location_;
    const std::size_t start = current_;
    ScanToken(start);
  }

  tokens_.push_back({.kind = TokenKind::EndOfFile, .lexeme = {}, .location = location_});
  return {.tokens = std::move(tokens_), .diagnostics = std::move(diagnostics_)};
}

void Lexer::ScanToken(std::size_t start) {
  const char character = Advance();
  switch (character) {
    case '{':
      AddToken(TokenKind::LeftBrace, start);
      break;
    case '}':
      AddToken(TokenKind::RightBrace, start);
      break;
    case '(':
      AddToken(TokenKind::LeftParen, start);
      break;
    case ')':
      AddToken(TokenKind::RightParen, start);
      break;
    case '[':
      AddToken(TokenKind::LeftBracket, start);
      break;
    case ']':
      AddToken(TokenKind::RightBracket, start);
      break;
    case ';':
      AddToken(TokenKind::Semicolon, start);
      break;
    case ',':
      AddToken(TokenKind::Comma, start);
      break;
    case ':':
      AddToken(TokenKind::Colon, start);
      break;
    case '.':
      AddToken(TokenKind::Dot, start);
      break;
    case '+':
      AddToken(TokenKind::Plus, start);
      break;
    case '-':
      AddToken(TokenKind::Minus, start);
      break;
    case '*':
      AddToken(TokenKind::Star, start);
      break;
    case '/':
      AddToken(TokenKind::Slash, start);
      break;
    case '=':
      AddToken(Match('=') ? TokenKind::EqualEqual : TokenKind::Equal, start);
      break;
    case '!':
      AddToken(Match('=') ? TokenKind::BangEqual : TokenKind::Bang, start);
      break;
    case '<':
      AddToken(Match('=') ? TokenKind::LessEqual : TokenKind::Less, start);
      break;
    case '>':
      AddToken(Match('=') ? TokenKind::GreaterEqual : TokenKind::Greater, start);
      break;
    case '&':
      if (Match('&')) {
        AddToken(TokenKind::AndAnd, start);
      } else {
        AddDiagnostic(token_location_, "expected '&' after '&'");
      }
      break;
    case '|':
      if (Match('|')) {
        AddToken(TokenKind::OrOr, start);
      } else {
        AddDiagnostic(token_location_, "expected '|' after '|'");
      }
      break;
    case '"':
      ScanString();
      break;
    default:
      if (IsIdentifierStart(character)) {
        ScanIdentifier();
      } else if (IsDigit(character)) {
        ScanNumber();
      } else {
        AddDiagnostic(token_location_, "unexpected character");
      }
      break;
  }
}

auto Lexer::IsAtEnd() const -> bool {
  return current_ >= source_.size();
}

auto Lexer::Peek() const -> char {
  return IsAtEnd() ? '\0' : source_.at(current_);
}

auto Lexer::PeekNext() const -> char {
  return current_ + 1 >= source_.size() ? '\0' : source_.at(current_ + 1);
}

auto Lexer::Advance() -> char {
  const char character = source_.at(current_++);
  if (character == '\n') {
    ++location_.line;
    location_.column = 1;
  } else {
    ++location_.column;
  }
  return character;
}

auto Lexer::Match(char expected) -> bool {
  if (Peek() != expected) {
    return false;
  }
  Advance();
  return true;
}

void Lexer::SkipWhitespaceAndComments() {
  while (!IsAtEnd()) {
    const char character = Peek();
    if (std::isspace(static_cast<unsigned char>(character)) != 0) {
      Advance();
      continue;
    }
    if (character == '/' && PeekNext() == '/') {
      while (!IsAtEnd() && Peek() != '\n') {
        Advance();
      }
      continue;
    }
    if (character == '/' && PeekNext() == '*') {
      Advance();
      Advance();
      bool closed = false;
      while (!IsAtEnd()) {
        if (Peek() == '*' && PeekNext() == '/') {
          Advance();
          Advance();
          closed = true;
          break;
        }
        Advance();
      }
      if (!closed) {
        AddDiagnostic(location_, "unterminated block comment");
      }
      continue;
    }
    break;
  }
}

void Lexer::ScanIdentifier() {
  const std::size_t start = current_ - 1;
  while (IsIdentifierPart(Peek())) {
    Advance();
  }
  const std::string_view text = source_.substr(start, current_ - start);
  const auto keyword = kKeywords.find(text);
  AddToken(keyword == kKeywords.end() ? TokenKind::Identifier : keyword->second, start);
}

void Lexer::ScanNumber() {
  const std::size_t start = current_ - 1;
  while (IsDigit(Peek())) {
    Advance();
  }

  TokenKind kind = TokenKind::IntegerLiteral;
  if (Peek() == '.' && IsDigit(PeekNext())) {
    kind = TokenKind::FloatLiteral;
    Advance();
    while (IsDigit(Peek())) {
      Advance();
    }
  }
  AddToken(kind, start);
}

void Lexer::ScanString() {
  const std::size_t start = current_ - 1;
  bool closed = false;
  while (!IsAtEnd()) {
    if (Peek() == '"') {
      Advance();
      closed = true;
      break;
    }
    if (Peek() == '\n') {
      AddDiagnostic(location_, "string literal cannot contain a raw newline");
      break;
    }
    if (Peek() == '\\') {
      Advance();
      if (IsAtEnd()) {
        break;
      }
      Advance();
    } else {
      Advance();
    }
  }

  if (!closed) {
    AddDiagnostic(token_location_, "unterminated string literal");
    return;
  }
  AddToken(TokenKind::StringLiteral, start);
}

void Lexer::AddToken(TokenKind kind, std::size_t start) {
  tokens_.push_back({
      .kind = kind,
      .lexeme = std::string(source_.substr(start, current_ - start)),
      .location = token_location_,
  });
}

void Lexer::AddDiagnostic(SourceLocation location, std::string message) {
  diagnostics_.push_back({.location = location, .message = std::move(message)});
}

}  // namespace vela::frontend
