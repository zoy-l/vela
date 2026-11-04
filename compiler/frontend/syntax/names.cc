#include "compiler/frontend/syntax/names.h"

#include <string_view>

#include "compiler/frontend/syntax/lexer.h"
#include "compiler/frontend/syntax/token.h"

namespace vela::frontend {

bool IsIdentifier(std::string_view text) {
  const auto result = Lexer(text).Tokenize();
  return result.diagnostics.empty() && result.tokens.size() == 2 &&
         result.tokens[0].kind == TokenKind::Identifier && result.tokens[0].lexeme == text;
}

bool IsMemberIdentifier(std::string_view text) {
  const auto result = Lexer(text).Tokenize();
  return result.diagnostics.empty() && result.tokens.size() == 2 &&
         IsMemberName(result.tokens[0].kind) && result.tokens[0].lexeme == text;
}

bool IsDeclarationIdentifier(std::string_view text) {
  const auto result = Lexer(text).Tokenize();
  return result.diagnostics.empty() && result.tokens.size() == 2 &&
         IsDeclarationName(result.tokens[0].kind) && result.tokens[0].lexeme == text;
}

bool IsNativeTypeKey(std::string_view text) {
  while (true) {
    const auto dot = text.find('.');
    if (!IsIdentifier(text.substr(0, dot))) {
      return false;
    }
    if (dot == std::string_view::npos) {
      return true;
    }
    text.remove_prefix(dot + 1);
  }
}

}  // namespace vela::frontend
