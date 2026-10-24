#pragma once

#include <string_view>
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
  explicit Lexer(std::string_view source) : source_(source) {}

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
