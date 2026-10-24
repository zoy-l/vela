#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/token.h"

namespace vela::frontend {

struct LexResult final {
  std::vector<Token> tokens;
  Diagnostics diagnostics;
};

class Lexer final {
 public:
  explicit Lexer(std::string_view source, std::uint32_t source_id = 0) : source_(source) {
    location_.source_id = source_id;
  }

  [[nodiscard]] auto Tokenize() -> LexResult;

 private:
  [[nodiscard]] auto IsAtEnd() const -> bool;
  [[nodiscard]] auto Peek() const -> char;
  [[nodiscard]] auto PeekNext() const -> char;
  auto Advance() -> char;
  auto Match(char expected) -> bool;
  void SkipWhitespaceAndComments();
  void ScanToken(std::size_t start);
  void ScanIdentifier();
  void ScanNumber();
  void ScanString();
  void ScanStringRun(std::string& value);
  auto ScanInterpolation() -> bool;
  void ScanEscape(std::string& value);
  void ScanUnicodeEscape(std::string& value, SourceLocation escape_location);
  auto ReadHexDigits(std::size_t limit) -> std::pair<std::uint32_t, std::size_t>;
  void AddToken(TokenKind kind, std::size_t start);
  void AddDiagnostic(SourceLocation location, std::string message);

  std::string_view source_;
  std::size_t current_ = 0;
  SourceLocation location_;
  SourceLocation token_location_;
  std::vector<Token> tokens_;
  Diagnostics diagnostics_;
};

}  // namespace vela::frontend
