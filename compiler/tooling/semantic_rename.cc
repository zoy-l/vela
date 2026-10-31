#include "compiler/tooling/semantic_index.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/frontend/lexer.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/standard_environment.h"
#include "compiler/frontend/token.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling {

std::optional<std::size_t> SemanticIndex::RenameTarget(std::size_t offset) const {
  const auto* token = QueryToken(offset);
  if (token == nullptr) {
    return std::nullopt;
  }
  const auto reference = references_.find({0, token->offset});
  if (reference == references_.end()) {
    return std::nullopt;
  }
  const auto& symbol = symbols_.at(reference->second.symbol);
  if ((symbol.role != Symbol::Role::TypeParameter &&
       symbol.completion.kind != CompletionKind::Variable &&
       symbol.completion.kind != CompletionKind::Constant) ||
      sources_.at(symbol.key.first).path.starts_with(frontend::kStandardSourcePrefix)) {
    return std::nullopt;
  }
  return reference->second.symbol;
}

std::optional<Definition> SemanticIndex::PrepareRename(std::size_t offset,
                                                       PositionEncoding encoding) const {
  if (!RenameTarget(offset)) {
    return std::nullopt;
  }
  const auto& token = *QueryToken(offset);
  const auto& source = sources_.front();
  return Definition{
      .path = source.path,
      .range =
          {
              .start = source.text.PositionAt(token.offset, encoding),
              .end = source.text.PositionAt(token.offset + token.length, encoding),
          },
  };
}

SemanticIndex::Key SemanticIndex::RenamedKey(Key key,
                                             const SemanticIndex& renamed,
                                             const SymbolRename& request) const {
  const auto& symbol = symbols_.at(request.symbol);
  const auto first = std::ranges::lower_bound(request.offsets, Key{key.first, 0});
  const auto last = std::ranges::lower_bound(request.offsets, key);
  const auto before = static_cast<std::size_t>(last - first);
  key.second = key.second + (before * request.name.size()) - (before * symbol.length);
  key.first = renamed.source_ids_.at(sources_.at(key.first).path);
  return key;
}

bool SemanticIndex::CanPreserveReferences(const SemanticIndex& renamed,
                                          const SymbolRename& request) const {
  if (references_.size() != renamed.references_.size()) {
    return false;
  }
  return std::ranges::all_of(references_, [&](const auto& entry) {
    const auto& [key, reference] = entry;
    const auto found = renamed.references_.find(RenamedKey(key, renamed, request));
    return found != renamed.references_.end() &&
           renamed.symbols_.at(found->second.symbol).key ==
               RenamedKey(symbols_.at(reference.symbol).key, renamed, request);
  });
}

std::expected<std::vector<Definition>, std::string> SemanticIndex::RenameSymbol(
    std::size_t offset,
    std::string_view name,
    PositionEncoding encoding) const {
  const auto lexed = frontend::Lexer(name).Tokenize();
  if (!lexed.diagnostics.empty() || lexed.tokens.size() != 2 ||
      lexed.tokens.front().kind != frontend::TokenKind::Identifier ||
      lexed.tokens.front().offset != 0 || lexed.tokens.front().length != name.size()) {
    return std::unexpected("new name must be a Vela identifier");
  }
  const auto selected = RenameTarget(offset);
  if (!selected) {
    return std::unexpected("rename requires a variable or type parameter declaration or reference");
  }
  const auto& symbol = symbols_.at(*selected);
  if (symbol.role == Symbol::Role::TypeParameter) {
    const auto selected_identity = std::ranges::find_if(
        type_parameter_symbols_, [&](const auto& entry) { return entry.second == *selected; });
    for (const auto& [identity, parameter] : type_parameter_symbols_) {
      if (parameter != *selected && symbols_.at(parameter).completion.label == name &&
          identity.owner == selected_identity->first.owner) {
        return std::unexpected("new name duplicates another type parameter");
      }
    }
  }
  SymbolRename request{.symbol = *selected, .offsets = {}, .name = name};
  for (const auto& [key, reference] : references_) {
    if (reference.symbol == *selected) {
      request.offsets.push_back(key);
    }
  }
  std::ranges::sort(request.offsets);
  std::vector<std::string> texts;
  texts.reserve(sources_.size());
  for (const auto& input : sources_) {
    texts.emplace_back(input.text.text());
  }
  for (const auto& [source, position] : std::views::reverse(request.offsets)) {
    texts.at(source).replace(position, symbol.length, name);
  }
  std::vector<frontend::SourceInput> overlays;
  overlays.reserve(sources_.size());
  for (std::size_t index = 0; index < sources_.size(); ++index) {
    overlays.push_back({.path = sources_.at(index).path, .text = texts.at(index)});
  }
  // 由生产绑定器重新解析改名后的身份；引用集合必须保持，避免遮蔽或捕获其它符号。
  const SemanticIndex renamed(sources_.front().path, overlays);
  if (!CanPreserveReferences(renamed, request)) {
    return std::unexpected("new name changes the binding of another reference");
  }
  std::vector<Definition> edits;
  edits.reserve(request.offsets.size());
  for (const auto& [source_id, position] : request.offsets) {
    const auto& source = sources_.at(source_id);
    edits.push_back({
        .path = source.path,
        .range =
            {
                .start = source.text.PositionAt(position, encoding),
                .end = source.text.PositionAt(position + symbol.length, encoding),
            },
    });
  }
  return edits;
}

}  // namespace vela::tooling
