#include "compiler/frontend/lexer.h"

#include <cctype>
#include <unordered_map>

namespace vela::frontend {
namespace {

const std::unordered_map<std::string_view, TokenKind> kKeywords = {
    {"class", TokenKind::Class},   {"enum", TokenKind::Enum}, {"false", TokenKind::False},
    {"return", TokenKind::Return}, {"true", TokenKind::True}, {"void", TokenKind::Void},
};

bool IsIdentifierStart(char character) {
  const unsigned char value = static_cast<unsigned char>(character);
  return std::isalpha(value) != 0 || character == '_';
}

bool IsIdentifierPart(char character) {
  const unsigned char value = static_cast<unsigned char>(character);
  return std::isalnum(value) != 0 || character == '_';
}

bool IsDigit(char character) {
  const unsigned char value = static_cast<unsigned char>(character);
  return std::isdigit(value) != 0;
}

}  // namespace

LexResult Lexer::Tokenize() {
  while (!IsAtEnd()) {
    SkipWhitespaceAndComments();
    if (IsAtEnd()) {
      break;
    }

    token_location_ = location_;
    const std::size_t start = current_;
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

  tokens_.push_back({TokenKind::EndOfFile, {}, location_});
  return {std::move(tokens_), std::move(diagnostics_)};
}

bool Lexer::IsAtEnd() const {
  return current_ >= source_.size();
}

char Lexer::Peek() const {
  return IsAtEnd() ? '\0' : source_[current_];
}

char Lexer::PeekNext() const {
  return current_ + 1 >= source_.size() ? '\0' : source_[current_ + 1];
}

char Lexer::Advance() {
  const char character = source_[current_++];
  if (character == '\n') {
    ++location_.line;
    location_.column = 1;
  } else {
    ++location_.column;
  }
  return character;
}

bool Lexer::Match(char expected) {
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
  const std::size_t start = current_;
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
  tokens_.push_back({kind, std::string(source_.substr(start, current_ - start)), token_location_});
}

void Lexer::AddDiagnostic(SourceLocation location, std::string message) {
  diagnostics_.push_back({location, std::move(message)});
}

}  // namespace vela::frontend
