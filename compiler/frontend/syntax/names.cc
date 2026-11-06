#include "compiler/frontend/syntax/names.h"

#include <algorithm>
#include <optional>
#include <string_view>

#include "compiler/frontend/syntax/lexer.h"
#include "compiler/frontend/syntax/token.h"

namespace vela::frontend {
namespace {

std::optional<TokenKind> NameKind(std::string_view text) {
  if (text.empty() || !IsIdentifierStart(text.front()) ||
      !std::ranges::all_of(text.substr(1), IsIdentifierPart)) {
    return std::nullopt;
  }
  return IdentifierKind(text);
}

}  // namespace

bool IsIdentifier(std::string_view text) {
  return NameKind(text) == TokenKind::Identifier;
}

bool IsMemberIdentifier(std::string_view text) {
  const auto kind = NameKind(text);
  return kind && IsMemberName(*kind);
}

bool IsDeclarationIdentifier(std::string_view text) {
  const auto kind = NameKind(text);
  return kind && IsDeclarationName(*kind);
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
