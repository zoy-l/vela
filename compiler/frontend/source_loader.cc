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
  const auto load = [&](const auto& self, const std::filesystem::path& file) -> void {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(file, error);
    if (error) {
      result.diagnostics.push_back({
          .location = SourceLocation(),
          .message = "cannot resolve source path: " + error.message(),
      });
      return;
    }
    const auto encoded = canonical.generic_u8string();
    const std::string key(encoded.begin(), encoded.end());
    const auto found = states.find(key);
    if (found != states.end()) {
      if (found->second == LoadState::Visiting) {
        result.diagnostics.push_back({
            .location = SourceLocation(),
            .message = "cyclic import involving '" + key + "'",
        });
      }
      return;
    }
    states.emplace(key, LoadState::Visiting);
    std::ifstream input(canonical, std::ios::binary);
    if (!input) {
      result.diagnostics.push_back({
          .location = SourceLocation(),
          .message = "cannot open source '" + key + "'",
      });
      states.at(key) = LoadState::Complete;
      return;
    }
    const std::string source((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    if (input.bad()) {
      result.diagnostics.push_back({
          .location = SourceLocation(),
          .message = "cannot read source '" + key + "'",
      });
      states.at(key) = LoadState::Complete;
      return;
    }
    auto parsed = ParseSource(source);
    if (!parsed.diagnostics.empty()) {
      for (auto& diagnostic : parsed.diagnostics) {
        diagnostic.message = "in source '" + key + "': " + diagnostic.message;
        result.diagnostics.push_back(std::move(diagnostic));
      }
      states.at(key) = LoadState::Complete;
      return;
    }
    for (const auto& imported : parsed.program.imports) {
      self(self, canonical.parent_path() / Utf8Path(imported));
    }
    AppendProgram(result.program, std::move(parsed.program));
    // 递归可能使哈希表 rehash；此处按 key 重新定位，不保留表内引用。
    states.at(key) = LoadState::Complete;
  };
  load(load, Utf8Path(path));
  return result;
}

}  // namespace vela::frontend
