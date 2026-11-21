#include "compiler/tooling/semantic/semantic_index.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/token.h"

namespace vela::tooling {

frontend::Diagnostics SemanticIndex::UnusedDiagnostics() const {
  frontend::Diagnostics result;
  if (sources_.empty() || frontend::HasErrors(diagnostics_)) {
    return result;
  }
  CollectUnusedParameters(result);
  CollectUnusedImports(result);
  std::ranges::sort(result, {}, [](const frontend::Diagnostic& diagnostic) {
    return std::pair(diagnostic.location.line, diagnostic.location.column);
  });
  return result;
}

void SemanticIndex::CollectUnusedParameters(frontend::Diagnostics& diagnostics) const {
  absl::flat_hash_set<std::size_t> used_parameters;
  for (const auto& [key, reference] : references_) {
    if (key.first == 0 && reference.expression != nullptr && reference.is_read) {
      used_parameters.insert(reference.symbol);
    }
  }
  const auto& source = sources_.front();
  const auto& tokens = source.tokens;
  for (const auto symbol_index : parameter_definitions_) {
    const auto& symbol = symbols_[symbol_index];
    const auto& name = symbol.completion.label;
    if (used_parameters.contains(symbol_index) || name.starts_with('_')) {
      continue;
    }
    const auto& token =
        *std::ranges::lower_bound(tokens, symbol.key.second, {}, &frontend::Token::offset);
    diagnostics.push_back({
        .location = token.location,
        .message = "parameter '" + name + "' is never read",
        .source_path = source.path,
        .severity = frontend::DiagnosticSeverity::Warning,
        .code = frontend::DiagnosticCode::UnusedParameter,
        .length = token.length,
    });
  }
}

void SemanticIndex::CollectUnusedImports(frontend::Diagnostics& diagnostics) const {
  const auto& source = sources_.front();
  const auto& tokens = source.tokens;
  using ImportKey = std::pair<std::uint32_t, std::string>;
  absl::flat_hash_map<ImportKey, std::size_t> imports;
  std::vector<bool> used_imports(source.imports.size());
  for (std::size_t index = 0; index < source.imports.size(); ++index) {
    const auto& imported = source.imports[index];
    if (const auto target = ImportSource(imported)) {
      imports.try_emplace(ImportKey{*target, imported.alias}, index);
    }
  }
  for (const auto& [key, reference] : references_) {
    if (key.first != 0) {
      continue;
    }
    const auto& symbol = symbols_[reference.symbol];
    if (symbol.key.first == 0 || !symbol.owner.empty()) {
      continue;
    }
    const auto token = std::ranges::lower_bound(tokens, key.second, {}, &frontend::Token::offset);
    const auto token_index = static_cast<std::size_t>(token - tokens.begin());
    std::string prefix;
    if (token_index >= 2 && tokens[token_index - 1].kind == frontend::TokenKind::Dot) {
      prefix = tokens[token_index - 2].lexeme;
    }
    const auto imported = imports.find(ImportKey{symbol.key.first, std::move(prefix)});
    if (imported != imports.end()) {
      used_imports[imported->second] = true;
    }
  }
  for (std::size_t index = 0; index < source.imports.size(); ++index) {
    const auto& imported = source.imports[index];
    if (used_imports[index] || !ImportSource(imported)) {
      continue;
    }
    const auto path_token = TokenAt(imported.location);
    const auto& first = tokens[path_token - 1];
    const auto& last = tokens[path_token + (imported.alias.empty() ? 1 : 3)];
    diagnostics.push_back({
        .location = first.location,
        .message = "unused import '" + imported.path + "'",
        .source_path = source.path,
        .severity = frontend::DiagnosticSeverity::Warning,
        .code = frontend::DiagnosticCode::UnusedImport,
        .length = last.offset + last.length - first.offset,
    });
  }
}

}  // namespace vela::tooling
