#pragma once

#include <string>

#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

enum class TokenKind {
  EndOfFile,
  Identifier,
  IntegerLiteral,
  FloatLiteral,
  StringLiteral,
  Class,
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
};

struct Token final {
  TokenKind kind = TokenKind::EndOfFile;
  std::string lexeme;
  SourceLocation location;
};

}  // namespace vela::frontend
