#include "compiler/frontend/source_loader.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/parser.h"

namespace vela::frontend {
namespace {

enum class LoadState : std::uint8_t { Visiting, Complete };

auto Utf8Path(std::string_view text) -> std::filesystem::path {
  return {std::u8string(text.begin(), text.end())};
}

}  // namespace

auto LoadProgram(std::string_view path) -> ParseResult {
  ParseResult result;
  absl::flat_hash_map<std::string, LoadState> states;
  const auto load = [&](const auto& self, const std::filesystem::path& file,
                        SourceLocation import_location, const std::string& importer) -> void {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(file, error);
    if (error) {
      result.diagnostics.push_back({
          .location = import_location,
          .message = "cannot resolve source path: " + error.message(),
          .source_path = importer,
      });
      return;
    }
    const auto encoded = canonical.generic_u8string();
    const std::string key(encoded.begin(), encoded.end());
    const auto found = states.find(key);
    if (found != states.end()) {
      if (found->second == LoadState::Visiting) {
        result.diagnostics.push_back({
            .location = import_location,
            .message = "cyclic import involving '" + key + "'",
            .source_path = importer,
        });
      }
      return;
    }
    states.emplace(key, LoadState::Visiting);
    std::ifstream input(canonical, std::ios::binary);
    if (!input) {
      result.diagnostics.push_back({
          .location = import_location,
          .message = "cannot open source '" + key + "'",
          .source_path = importer.empty() ? key : importer,
      });
      states.at(key) = LoadState::Complete;
      return;
    }
    const std::string source((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    if (input.bad()) {
      result.diagnostics.push_back({
          .location = import_location,
          .message = "cannot read source '" + key + "'",
          .source_path = importer.empty() ? key : importer,
      });
      states.at(key) = LoadState::Complete;
      return;
    }
    const auto source_id = static_cast<std::uint32_t>(result.program.source_paths.size());
    result.program.source_paths.push_back(key);
    auto parsed = ParseSource(source, source_id);
    if (!parsed.diagnostics.empty()) {
      for (auto& diagnostic : parsed.diagnostics) {
        diagnostic.source_path = key;
        result.diagnostics.push_back(std::move(diagnostic));
      }
      states.at(key) = LoadState::Complete;
      return;
    }
    for (const auto& imported : parsed.program.imports) {
      self(self, canonical.parent_path() / Utf8Path(imported.path), imported.location, key);
    }
    AppendProgram(result.program, std::move(parsed.program));
    // 递归可能使哈希表 rehash；此处按 key 重新定位，不保留表内引用。
    states.at(key) = LoadState::Complete;
  };
  load(load, Utf8Path(path), SourceLocation(), std::string(path));
  return result;
}

}  // namespace vela::frontend
