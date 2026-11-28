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
    case CompletionKind::Struct:
      return SemanticTokenKind::Struct;
    case CompletionKind::Interface:
      return SemanticTokenKind::Interface;
    case CompletionKind::Enum:
      return SemanticTokenKind::Enum;
    case CompletionKind::EnumMember:
      return SemanticTokenKind::EnumMember;
    case CompletionKind::Type:
      return SemanticTokenKind::Type;
    case CompletionKind::Variable:
    case CompletionKind::Constant:
      return SemanticTokenKind::Variable;
    default:
      return std::nullopt;
  }
}

}  // namespace

std::optional<SemanticToken> SemanticIndex::SemanticTokenFor(const frontend::Token& token,
                                                             PositionEncoding encoding) const {
  if (token.kind != frontend::TokenKind::Identifier && token.kind != frontend::TokenKind::Void) {
    return std::nullopt;
  }
  const auto& source = sources_.front();
  const Key key{0, token.offset};
  const bool is_declaration = declarations_.contains(key);
  auto modifiers = is_declaration ? kSemanticDeclaration : std::uint32_t{0};
  std::optional<SemanticTokenKind> kind;
  if (const auto found = references_.find(key); found != references_.end()) {
    const auto& reference = found->second;
    const auto& symbol = symbols_[reference.symbol];
    if (symbol.role == Symbol::Role::TypeParameter) {
      kind = SemanticTokenKind::TypeParameter;
    } else if (symbol.role == Symbol::Role::Parameter) {
      // 命名实参的标签也指向参数声明，但不是对参数值的读取。
      if (is_declaration || reference.expression != nullptr) {
        kind = SemanticTokenKind::Parameter;
      }
    } else {
      kind = TokenKindFor(symbol.completion.kind);
    }
    if (symbol.completion.kind == CompletionKind::Constant) {
      modifiers |= kSemanticReadonly;
    }
    if (sources_[symbol.key.first].path.starts_with(frontend::kStandardSourcePrefix)) {
      modifiers |= kSemanticDefaultLibrary;
    }
  } else if (const auto detail = BuiltinTypeDetail(token.lexeme); !detail.empty()) {
    kind = detail.starts_with("class ") ? SemanticTokenKind::Class : SemanticTokenKind::Type;
    modifiers |= kSemanticDefaultLibrary;
  }
  if (!kind) {
    return std::nullopt;
  }
  const auto range = source.lines.RangeAt(source.text, token.offset, token.length, encoding);
  return SemanticToken{
      .position = range.start,
      .length = range.end.character - range.start.character,
      .kind = *kind,
      .modifiers = modifiers,
  };
}

std::vector<SemanticToken> SemanticIndex::SemanticTokens(PositionEncoding encoding) const {
  std::vector<SemanticToken> result;
  if (sources_.empty()) {
    return result;
  }
  for (const auto& token : sources_.front().tokens) {
    if (const auto semantic = SemanticTokenFor(token, encoding)) {
      result.push_back(*semantic);
    }
  }
  return result;
}

}  // namespace vela::tooling
