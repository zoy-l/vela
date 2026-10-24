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

  [[nodiscard]] LexResult Tokenize();

 private:
  [[nodiscard]] bool IsAtEnd() const;
  [[nodiscard]] char Peek() const;
  [[nodiscard]] char PeekNext() const;
  char Advance();
  bool Match(char expected);
  void SkipWhitespaceAndComments();
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
