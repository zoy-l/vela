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
    {"type", TokenKind::Type},
    {"private", TokenKind::Private},
    {"static", TokenKind::Static},
    {"constructor", TokenKind::Constructor},
    {"extends", TokenKind::Extends},
    {"super", TokenKind::Super},
    {"override", TokenKind::Override},
    {"get", TokenKind::Get},
    {"set", TokenKind::Set},
    {"interface", TokenKind::Interface},
    {"implements", TokenKind::Implements},
    {"is", TokenKind::Is},
    {"as", TokenKind::As},
    {"match", TokenKind::Match},
    {"func", TokenKind::Func},
    {"async", TokenKind::Async},
    {"await", TokenKind::Await},
    {"let", TokenKind::Let},
    {"const", TokenKind::Const},
    {"class", TokenKind::Class},
    {"else", TokenKind::Else},
    {"enum", TokenKind::Enum},
    {"false", TokenKind::False},
    {"if", TokenKind::If},
    {"return", TokenKind::Return},
    {"true", TokenKind::True},
    {"void", TokenKind::Void},
    {"while", TokenKind::While},
    {"import", TokenKind::Import},
    {"export", TokenKind::Export},
    {"external", TokenKind::External},
    {"try", TokenKind::Try},
    {"catch", TokenKind::Catch},
    {"finally", TokenKind::Finally},
    {"throw", TokenKind::Throw},
    {"break", TokenKind::Break},
    {"continue", TokenKind::Continue},
    {"for", TokenKind::For},
    {"in", TokenKind::In},
    {"print", TokenKind::Print},
    {"switch", TokenKind::Switch},
    {"case", TokenKind::Case},
    {"default", TokenKind::Default},
    {"null", TokenKind::Null},
};

bool IsIdentifierStart(char character) {
  const auto value = static_cast<unsigned char>(character);
  return std::isalpha(value) != 0 || character == '_';
}

bool IsIdentifierPart(char character) {
  const auto value = static_cast<unsigned char>(character);
  return std::isalnum(value) != 0 || character == '_';
}

bool IsDigit(char character) {
  const auto value = static_cast<unsigned char>(character);
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
    ScanToken(start);
  }

  tokens_.push_back({
      .lexeme = {},
      .offset = current_,
      .length = 0,
      .location = location_,
      .kind = TokenKind::EndOfFile,
  });
  return {
      .tokens = std::move(tokens_),
      .diagnostics = std::move(diagnostics_),
      .comments = std::move(comments_),
  };
}

void Lexer::ScanToken(std::size_t start) {
  const char character = Advance();
  if (ScanPunctuation(character, start)) {
    return;
  }
  if (IsIdentifierStart(character)) {
    ScanIdentifier();
    return;
  }
  if (IsDigit(character)) {
    ScanNumber();
    return;
  }
  AddDiagnostic(token_location_, "unexpected character");
}

bool Lexer::ScanPunctuation(char character, std::size_t start) {
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
      ScanQuestion(start);
      break;
    case '.':
      AddToken(TokenKind::Dot, start);
      break;
    case '+':
    case '-':
    case '*':
    case '/':
    case '%':
      ScanArithmetic({.character = character, .start = start});
      break;
    case '=':
    case '<':
    case '>':
      ScanComparison({.character = character, .start = start});
      break;
    case '!':
      AddToken(Match('=') ? TokenKind::BangEqual : TokenKind::Bang, start);
      break;
    case '&':
    case '|':
    case '^':
    case '~':
      ScanBitwise({.character = character, .start = start});
      break;
    case '"':
    case '\'':
      ScanString(character);
      return true;
    default:
      return false;
  }
  return true;
}

void Lexer::ScanQuestion(std::size_t start) {
  if (Match('.')) {
    AddToken(TokenKind::QuestionDot, start);
  } else if (Match('?')) {
    AddToken(TokenKind::QuestionQuestion, start);
  } else {
    AddToken(TokenKind::Question, start);
  }
}

void Lexer::ScanArithmetic(PunctuationContext context) {
  const auto [character, start] = context;
  if (character == '+') {
    if (Match('+')) {
      AddToken(TokenKind::PlusPlus, start);
    } else {
      AddToken(Match('=') ? TokenKind::PlusEqual : TokenKind::Plus, start);
    }
  } else if (character == '-') {
    if (Match('-')) {
      AddToken(TokenKind::MinusMinus, start);
    } else {
      AddToken(Match('=') ? TokenKind::MinusEqual : TokenKind::Minus, start);
    }
  } else if (character == '*') {
    AddToken(Match('=') ? TokenKind::StarEqual : TokenKind::Star, start);
  } else if (character == '/') {
    AddToken(Match('=') ? TokenKind::SlashEqual : TokenKind::Slash, start);
  } else {
    AddToken(Match('=') ? TokenKind::PercentEqual : TokenKind::Percent, start);
  }
}

void Lexer::ScanComparison(PunctuationContext context) {
  const auto [character, start] = context;
  switch (character) {
    case '=':
      ScanEquals(start);
      return;
    case '<':
      ScanLess(start);
      return;
    default:
      ScanGreater(start);
      return;
  }
}

void Lexer::ScanEquals(std::size_t start) {
  if (Match('=')) {
    AddToken(TokenKind::EqualEqual, start);
  } else if (Match('>')) {
    AddToken(TokenKind::Arrow, start);
  } else {
    AddToken(TokenKind::Equal, start);
  }
}

void Lexer::ScanLess(std::size_t start) {
  if (Match('<')) {
    AddToken(Match('=') ? TokenKind::ShiftLeftEqual : TokenKind::ShiftLeft, start);
  } else {
    AddToken(Match('=') ? TokenKind::LessEqual : TokenKind::Less, start);
  }
}

void Lexer::ScanGreater(std::size_t start) {
  if (Match('>')) {
    if (Match('>')) {
      AddToken(Match('=') ? TokenKind::ShiftUnsignedRightEqual : TokenKind::ShiftUnsignedRight,
               start);
    } else {
      AddToken(Match('=') ? TokenKind::ShiftRightEqual : TokenKind::ShiftRight, start);
    }
  } else {
    AddToken(Match('=') ? TokenKind::GreaterEqual : TokenKind::Greater, start);
  }
}

void Lexer::ScanBitwise(PunctuationContext context) {
  const auto [character, start] = context;
  if (character == '&') {
    if (Match('&')) {
      AddToken(TokenKind::AndAnd, start);
    } else {
      AddToken(Match('=') ? TokenKind::AmpersandEqual : TokenKind::Ampersand, start);
    }
  } else if (character == '|') {
    if (Match('|')) {
      AddToken(TokenKind::OrOr, start);
    } else {
      AddToken(Match('=') ? TokenKind::PipeEqual : TokenKind::Pipe, start);
    }
  } else if (character == '^') {
    AddToken(Match('=') ? TokenKind::CaretEqual : TokenKind::Caret, start);
  } else {
    AddToken(TokenKind::Tilde, start);
  }
}

bool Lexer::IsAtEnd() const {
  return current_ >= source_.size();
}

char Lexer::Peek() const {
  return IsAtEnd() ? '\0' : source_.at(current_);
}

char Lexer::PeekNext() const {
  return current_ + 1 >= source_.size() ? '\0' : source_.at(current_ + 1);
}

char Lexer::Advance() {
  const char character = source_.at(current_++);
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
      const auto start = current_;
      const auto opening = location_;
      while (!IsAtEnd() && Peek() != '\n') {
        Advance();
      }
      RecordComment(opening, start);
      continue;
    }
    if (character == '/' && PeekNext() == '*') {
      SkipBlockComment();
      continue;
    }
    break;
  }
}

void Lexer::RecordComment(SourceLocation location, std::size_t start) {
  if (comment_retention_ == CommentRetention::Preserve) {
    comments_.push_back({.location = location, .offset = start, .length = current_ - start});
  }
}

void Lexer::SkipBlockComment() {
  const auto start = current_;
  const auto opening = location_;
  Advance();
  Advance();
  while (!IsAtEnd()) {
    if (Peek() == '*' && PeekNext() == '/') {
      Advance();
      Advance();
      RecordComment(opening, start);
      return;
    }
    Advance();
  }
  AddDiagnostic(opening, "unterminated block comment");
  RecordComment(opening, start);
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

void Lexer::ScanString(char quote) {
  const auto opening = token_location_;
  auto segment_location = opening;
  auto segment_start = current_ - 1;
  std::string value;
  bool interpolated = false;
  while (!IsAtEnd()) {
    if (Peek() == quote) {
      Advance();
      tokens_.push_back({
          .lexeme = std::move(value),
          .offset = segment_start,
          .length = current_ - segment_start,
          .location = segment_location,
          .kind = interpolated ? TokenKind::StringTail : TokenKind::StringLiteral,
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
          .lexeme = std::move(value),
          .offset = segment_start,
          .length = current_ - segment_start,
          .location = segment_location,
          .kind = interpolated ? TokenKind::StringMiddle : TokenKind::StringHead,
      });
      interpolated = true;
      value.clear();
      if (!ScanInterpolation()) {
        return;
      }
      segment_location = location_;
      segment_start = current_;
    } else {
      ScanStringRun(value, quote);
    }
  }
  AddDiagnostic(opening, "unterminated string literal");
}

void Lexer::ScanStringRun(std::string& value, char quote) {
  const auto start = current_;
  const auto start_location = location_;
  while (!IsAtEnd()) {
    const char character = Peek();
    if (character == quote || character == '\\' || character == '\n' || character == '\r' ||
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

bool Lexer::ScanInterpolation() {
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

std::pair<std::uint32_t, std::size_t> Lexer::ReadHexDigits(std::size_t limit) {
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
      .lexeme = std::string(source_.substr(start, current_ - start)),
      .offset = start,
      .length = current_ - start,
      .location = token_location_,
      .kind = kind,
  });
}

void Lexer::AddDiagnostic(SourceLocation location, std::string message) {
  diagnostics_.push_back({.location = location, .message = std::move(message), .source_path = {}});
}

}  // namespace vela::frontend
