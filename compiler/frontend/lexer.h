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

enum class CommentRetention : std::uint8_t { Discard, Preserve };

struct LexResult final {
  std::vector<Token> tokens;
  Diagnostics diagnostics;
  std::vector<SourceComment> comments;
};

class Lexer final {
 public:
  explicit Lexer(std::string_view source,
                 std::uint32_t source_id = 0,
                 CommentRetention comments = CommentRetention::Discard)
      : source_(source), comment_retention_(comments) {
    location_.source_id = source_id;
  }

  [[nodiscard]] LexResult Tokenize();

 private:
  [[nodiscard]] bool IsAtEnd() const;
  [[nodiscard]] char Peek() const;
  [[nodiscard]] char PeekNext() const;
  char Advance();
  bool Match(char expected);
  void SkipWhitespaceAndComments();
  void SkipBlockComment();
  void RecordComment(SourceLocation location, std::size_t start);
  void ScanToken(std::size_t start);
  [[nodiscard]] bool ScanPunctuation(char character, std::size_t start);
  struct PunctuationContext final {
    char character;
    std::size_t start;
  };
  void ScanQuestion(std::size_t start);
  void ScanArithmetic(PunctuationContext context);
  void ScanComparison(PunctuationContext context);
  void ScanEquals(std::size_t start);
  void ScanLess(std::size_t start);
  void ScanGreater(std::size_t start);
  void ScanBitwise(PunctuationContext context);
  void ScanIdentifier();
  void ScanNumber();
  void ScanString(char quote);
  void ScanStringRun(std::string& value, char quote);
  bool ScanInterpolation();
  void ScanEscape(std::string& value);
  void ScanUnicodeEscape(std::string& value, SourceLocation escape_location);
  std::pair<std::uint32_t, std::size_t> ReadHexDigits(std::size_t limit);
  void AddToken(TokenKind kind, std::size_t start);
  void AddDiagnostic(SourceLocation location, std::string message);

  std::string_view source_;
  std::size_t current_ = 0;
  SourceLocation location_;
  SourceLocation token_location_;
  std::vector<Token> tokens_;
  Diagnostics diagnostics_;
  CommentRetention comment_retention_;
  std::vector<SourceComment> comments_;
};

}  // namespace vela::frontend
