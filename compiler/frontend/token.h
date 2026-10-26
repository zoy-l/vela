#pragma once

#include <cstddef>
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
  StringHead,
  StringMiddle,
  StringTail,
  Class,
  Func,
  Async,
  Await,
  Type,
  Public,
  Private,
  Static,
  Param,
  Constructor,
  Extends,
  Super,
  Override,
  Get,
  Set,
  Interface,
  Implements,
  Is,
  As,
  Match,
  Arrow,
  Let,
  Const,
  Import,
  Export,
  External,
  Try,
  Catch,
  Finally,
  Throw,
  Break,
  Continue,
  For,
  In,
  Print,
  Else,
  Enum,
  False,
  If,
  Return,
  True,
  Void,
  While,
  Switch,
  Case,
  Default,
  Null,
  Question,
  QuestionDot,
  QuestionLeftBracket,
  QuestionQuestion,
  LeftBrace,
  RightBrace,
  LeftParen,
  RightParen,
  Semicolon,
  Comma,
  Colon,
  Dot,
  Equal,
  PlusEqual,
  MinusEqual,
  StarEqual,
  SlashEqual,
  Percent,
  PercentEqual,
  EqualEqual,
  Bang,
  BangEqual,
  Plus,
  PlusPlus,
  Minus,
  MinusMinus,
  Star,
  Slash,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  ShiftLeft,
  ShiftLeftEqual,
  ShiftRight,
  ShiftRightEqual,
  ShiftUnsignedRight,
  ShiftUnsignedRightEqual,
  Ampersand,
  AmpersandEqual,
  Caret,
  CaretEqual,
  Pipe,
  PipeEqual,
  Tilde,
  AndAnd,
  OrOr,
  LeftBracket,
  RightBracket,
};

enum class TokenRole : std::uint8_t {
  Ordinary,
  TypeDelimiter,
  PrefixOperator,
  ConditionalOperator,
};

struct Token final {
  // 字符串 token 保存已解码的 UTF-8 内容，其他 token 保存源码文本。
  std::string lexeme;
  // 原始源码的字节范围，字符串转义解码后仍能准确定位原文。
  std::size_t offset = 0;
  std::size_t length = 0;
  SourceLocation location;
  TokenKind kind = TokenKind::EndOfFile;
  TokenRole role = TokenRole::Ordinary;
};

struct SourceComment final {
  SourceLocation location;
  std::size_t offset = 0;
  std::size_t length = 0;
};

}  // namespace vela::frontend
