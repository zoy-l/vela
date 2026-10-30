#include "compiler/development_files.h"

#include <algorithm>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/file.h"
#include "base/path.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/source_session.h"

namespace vela::compiler {
namespace {

frontend::Diagnostics FileError(const std::string& path, std::string message) {
  return {{.location = {}, .message = std::move(message), .source_path = path}};
}

std::expected<absl::flat_hash_map<std::string, std::string>, frontend::Diagnostics> ReadKnownFiles(
    const absl::flat_hash_set<std::string>& paths,
    frontend::ModuleCache& cache) {
  absl::flat_hash_map<std::string, std::string> texts;
  for (const auto& path : paths) {
    std::error_code error;
    const auto file = Utf8Path(path);
    const auto exists = std::filesystem::exists(file, error);
    if (error) {
      return std::unexpected(FileError(path, error.message()));
    }
    if (!exists) {
      cache.sources.erase(path);
      continue;
    }
    const auto bytes = ReadBinaryFile(file);
    if (!bytes) {
      return std::unexpected(FileError(path, bytes.error()));
    }
    const void* storage = bytes->data();
    texts.emplace(path, std::string(static_cast<const char*>(storage), bytes->size()));
  }
  return texts;
}

}  // namespace

DevelopmentFiles::DevelopmentFiles(std::string entry) : entry_(std::move(entry)) {}

std::expected<SourceUpdate, frontend::Diagnostics> DevelopmentFiles::Capture(
    const CompiledSourceSnapshot& previous) {
  absl::flat_hash_set<std::string> paths{entry_};
  for (const auto& [path, source] : cache_.sources) {
    paths.insert(path);
  }
  for (const auto& [path, text] : previous.sources) {
    if (!previous.prepared_sources.contains(path)) {
      paths.insert(path);
    }
  }
  auto read = ReadKnownFiles(paths, cache_);
  if (!read) {
    return std::unexpected(std::move(read.error()));
  }
  const auto& texts = *read;
  std::vector<frontend::SourceInput> inputs;
  for (const auto& [path, text] : texts) {
    inputs.push_back({.path = path, .text = text});
  }
  std::vector<std::string> roots{entry_};
  for (const auto& [path, text] : previous.sources) {
    if (path != entry_ && texts.contains(path) && !previous.prepared_sources.contains(path)) {
      roots.push_back(path);
    }
  }
  // 文件解析缓存不固定路径解析；symlink 等文件身份变化在本次捕获中重新确认。
  cache_.resolutions.clear();
  auto graph = frontend::LoadModuleGraph(roots, cache_, inputs);
  if (!graph.diagnostics.empty()) {
    return std::unexpected(std::move(graph.diagnostics));
  }
  entry_ = graph.sources.front().path;
  SourceUpdate update;
  update.base_revision = previous.revision;
  absl::flat_hash_set<std::string> captured;
  const absl::flat_hash_set<std::string> accepted_roots(previous.roots.begin(),
                                                        previous.roots.end());
  for (const auto& source : graph.sources) {
    captured.insert(source.path);
    const auto old = previous.sources.find(source.path);
    if (old == previous.sources.end() || *old->second != source.text) {
      update.changes.push_back({.path = source.path, .text = std::string(source.text)});
    }
    if (!accepted_roots.contains(source.path)) {
      update.added_roots.push_back(source.path);
    }
  }
  for (const auto& [path, text] : previous.sources) {
    if (!captured.contains(path) && !previous.prepared_sources.contains(path)) {
      update.changes.push_back({.path = path, .text = std::nullopt});
    }
  }
  std::ranges::sort(update.changes, {}, &SourceChange::path);
  std::ranges::sort(update.added_roots);
  return update;
}

}  // namespace vela::compiler
