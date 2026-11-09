#include "compiler/tooling/semantic/semantic_index.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/tooling/documentation/builtin_documentation.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling {
namespace {

std::optional<SemanticTokenKind> TokenKindFor(CompletionKind kind) {
  switch (kind) {
    case CompletionKind::Class:
      return SemanticTokenKind::Class;
    case CompletionKind::Interface:
      return SemanticTokenKind::Interface;
    case CompletionKind::Enum:
      return SemanticTokenKind::Enum;
    case CompletionKind::Type:
      return SemanticTokenKind::Type;
    default:
      return std::nullopt;
  }
}

}  // namespace

std::vector<SemanticToken> SemanticIndex::SemanticTokens(PositionEncoding encoding) const {
  std::vector<SemanticToken> result;
  if (sources_.empty()) {
    return result;
  }
  const auto& source = sources_.front();
  for (const auto& token : source.tokens) {
    if (token.kind != frontend::TokenKind::Identifier && token.kind != frontend::TokenKind::Void) {
      continue;
    }
    const Key key{0, token.offset};
    auto modifiers = declarations_.contains(key) ? kSemanticDeclaration : std::uint32_t{0};
    std::optional<SemanticTokenKind> kind;
    if (const auto found = references_.find(key); found != references_.end()) {
      const auto& symbol = symbols_[found->second.symbol];
      kind = symbol.role == Symbol::Role::TypeParameter ? SemanticTokenKind::TypeParameter
                                                        : TokenKindFor(symbol.completion.kind);
      if (sources_[symbol.key.first].path.starts_with(frontend::kStandardSourcePrefix)) {
        modifiers |= kSemanticDefaultLibrary;
      }
    } else if (const auto detail = BuiltinTypeDetail(token.lexeme); !detail.empty()) {
      kind = detail.starts_with("class ") ? SemanticTokenKind::Class : SemanticTokenKind::Type;
      modifiers |= kSemanticDefaultLibrary;
    }
    if (kind) {
      const auto range = source.lines.RangeAt(source.text, token.offset, token.length, encoding);
      result.push_back({
          .position = range.start,
          .length = range.end.character - range.start.character,
          .kind = *kind,
          .modifiers = modifiers,
      });
    }
  }
  return result;
}

}  // namespace vela::tooling
