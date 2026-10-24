#include "compiler/frontend/lexer.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include "base/utf8.h"
#include "compiler/frontend/token.h"

#include <absl/container/flat_hash_map.h>

namespace vela::frontend {
namespace {

constexpr std::uint32_t kHexLetterOffset = 10;
constexpr std::size_t kBracedUnicodeDigits = 6;

const absl::flat_hash_map<std::string_view, TokenKind> kKeywords = {
    {"func", TokenKind::Func},         {"let", TokenKind::Let},
    {"const", TokenKind::Const},       {"class", TokenKind::Class},
    {"else", TokenKind::Else},         {"enum", TokenKind::Enum},
    {"false", TokenKind::False},       {"if", TokenKind::If},
    {"return", TokenKind::Return},     {"true", TokenKind::True},
    {"void", TokenKind::Void},         {"while", TokenKind::While},
    {"import", TokenKind::Import},     {"break", TokenKind::Break},
    {"continue", TokenKind::Continue}, {"for", TokenKind::For},
    {"print", TokenKind::Print},       {"switch", TokenKind::Switch},
    {"case", TokenKind::Case},         {"default", TokenKind::Default},
    {"null", TokenKind::Null},
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
    case '?':
      AddToken(TokenKind::Question, start);
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
      const auto opening = location_;
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
        AddDiagnostic(opening, "unterminated block comment");
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
    kind = TokenKind::DoubleLiteral;
    Advance();
    while (IsDigit(Peek())) {
      Advance();
    }
  }
  if (Peek() == 'e' || Peek() == 'E') {
    kind = TokenKind::DoubleLiteral;
    Advance();
    if (Peek() == '+' || Peek() == '-') {
      Advance();
    }
    if (!IsDigit(Peek())) {
      AddDiagnostic(token_location_, "double exponent requires digits");
      return;
    }
    while (IsDigit(Peek())) {
      Advance();
    }
  }
  AddToken(kind, start);
}

void Lexer::ScanString() {
  const auto opening = token_location_;
  auto segment_location = opening;
  std::string value;
  bool interpolated = false;
  while (!IsAtEnd()) {
    if (Peek() == '"') {
      Advance();
      tokens_.push_back({
          .kind = interpolated ? TokenKind::StringTail : TokenKind::StringLiteral,
          .lexeme = std::move(value),
          .location = segment_location,
      });
      return;
    }
    if (Peek() == '\n' || Peek() == '\r') {
      AddDiagnostic(location_, "string literal cannot contain a raw newline");
      break;
    }
    if (Peek() == '\\') {
      Advance();
      ScanEscape(value);
    } else if (Peek() == '$' && PeekNext() == '{') {
      Advance();
      Advance();
      tokens_.push_back({
          .kind = interpolated ? TokenKind::StringMiddle : TokenKind::StringHead,
          .lexeme = std::move(value),
          .location = segment_location,
      });
      interpolated = true;
      value.clear();
      if (!ScanInterpolation()) {
        return;
      }
      segment_location = location_;
    } else {
      ScanStringRun(value);
    }
  }
  AddDiagnostic(opening, "unterminated string literal");
}

void Lexer::ScanStringRun(std::string& value) {
  const auto start = current_;
  const auto start_location = location_;
  while (!IsAtEnd()) {
    const char character = Peek();
    if (character == '"' || character == '\\' || character == '\n' || character == '\r' ||
        (character == '$' && PeekNext() == '{')) {
      break;
    }
    Advance();
  }
  const auto run = source_.substr(start, current_ - start);
  if (const auto invalid = InvalidUtf8Offset(run)) {
    auto error_location = start_location;
    error_location.column += static_cast<std::uint32_t>(*invalid);
    AddDiagnostic(error_location, "invalid UTF-8 in string literal");
  } else {
    value.append(run);
  }
}

auto Lexer::ScanInterpolation() -> bool {
  const auto opening = location_;
  std::size_t depth = 0;
  while (!IsAtEnd()) {
    SkipWhitespaceAndComments();
    if (IsAtEnd()) {
      break;
    }
    if (Peek() == '}' && depth == 0) {
      Advance();
      return true;
    }
    if (Peek() == '{') {
      ++depth;
    } else if (Peek() == '}') {
      --depth;
    }
    token_location_ = location_;
    ScanToken(current_);
  }
  AddDiagnostic(opening, "unterminated string interpolation");
  return false;
}

void Lexer::ScanEscape(std::string& value) {
  const auto escape_location = location_;
  if (IsAtEnd()) {
    return;
  }
  const char escaped = Advance();
  switch (escaped) {
    case 'n':
      value.push_back('\n');
      return;
    case 'r':
      value.push_back('\r');
      return;
    case 't':
      value.push_back('\t');
      return;
    case 'b':
      value.push_back('\b');
      return;
    case 'f':
      value.push_back('\f');
      return;
    case 'v':
      value.push_back('\v');
      return;
    case '0':
      value.push_back('\0');
      return;
    case '\\':
      value.push_back('\\');
      return;
    case '"':
      value.push_back('"');
      return;
    case '\'':
      value.push_back('\'');
      return;
    case '$':
      value.push_back('$');
      return;
    case 'x': {
      const auto [point, digits] = ReadHexDigits(2);
      if (digits != 2) {
        AddDiagnostic(escape_location, "invalid hexadecimal escape");
      } else {
        AppendUtf8(point, value);
      }
      return;
    }
    case 'u':
      break;
    default:
      AddDiagnostic(escape_location, "invalid string escape");
      return;
  }
  ScanUnicodeEscape(value, escape_location);
}

auto Lexer::ReadHexDigits(std::size_t limit) -> std::pair<std::uint32_t, std::size_t> {
  std::uint32_t point = 0;
  std::size_t digits = 0;
  while (!IsAtEnd() && digits < limit) {
    const char character = Peek();
    std::uint32_t digit = 0;
    if (character >= '0' && character <= '9') {
      digit = static_cast<std::uint32_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      digit = static_cast<std::uint32_t>(character - 'a') + kHexLetterOffset;
    } else if (character >= 'A' && character <= 'F') {
      digit = static_cast<std::uint32_t>(character - 'A') + kHexLetterOffset;
    } else {
      break;
    }
    Advance();
    point = (point << 4) | digit;
    ++digits;
  }
  return {point, digits};
}

void Lexer::ScanUnicodeEscape(std::string& value, SourceLocation escape_location) {
  const bool braced = Match('{');
  auto [point, digits] = ReadHexDigits(braced ? kBracedUnicodeDigits : 4);
  bool valid = braced ? digits != 0 && Match('}') : digits == 4;
  if (valid && !braced && point >= kHighSurrogateFirst && point <= kHighSurrogateLast) {
    valid = Match('\\') && Match('u');
    if (valid) {
      const auto [low, low_digits] = ReadHexDigits(4);
      valid = low_digits == 4 && low >= kLowSurrogateFirst && low <= kLowSurrogateLast;
      if (valid) {
        point = kSupplementaryFirst + ((point - kHighSurrogateFirst) << kSurrogatePayloadBits) +
                (low - kLowSurrogateFirst);
      }
    }
  }
  if (!valid || point > kUnicodeMaximum ||
      (point >= kHighSurrogateFirst && point <= kLowSurrogateLast)) {
    AddDiagnostic(escape_location, "invalid Unicode escape");
    return;
  }
  AppendUtf8(point, value);
}

void Lexer::AddToken(TokenKind kind, std::size_t start) {
  tokens_.push_back({
      .kind = kind,
      .lexeme = std::string(source_.substr(start, current_ - start)),
      .location = token_location_,
  });
}

void Lexer::AddDiagnostic(SourceLocation location, std::string message) {
  diagnostics_.push_back({.location = location, .message = std::move(message), .source_path = {}});
}

}  // namespace vela::frontend
