#pragma once

#include <cstdint>

#include <string>

#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

enum class TokenKind : std::uint8_t {
  EndOfFile,
  Identifier,
  IntegerLiteral,
  DoubleLiteral,
  StringLiteral,
  Class,
  Import,
  Break,
  Continue,
  For,
  Print,
  Else,
  Enum,
  False,
  If,
  Return,
  True,
  Void,
  While,
  LeftBrace,
  RightBrace,
  LeftParen,
  RightParen,
  Semicolon,
  Comma,
  Colon,
  Dot,
  Equal,
  EqualEqual,
  Bang,
  BangEqual,
  Plus,
  Minus,
  Star,
  Slash,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  AndAnd,
  OrOr,
  LeftBracket,
  RightBracket,
};

struct Token final {
  TokenKind kind = TokenKind::EndOfFile;
  std::string lexeme;
  SourceLocation location;
};

}  // namespace vela::frontend
