#include "compiler/frontend/source_loader.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/file.h"
#include "base/path.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/module_binding.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_file.h"
#include "compiler/frontend/standard_modules.h"

namespace vela::frontend {
namespace {

enum class SourceOrigin : std::uint8_t { Files, Memory };

class ModuleLoader final {
 public:
  ModuleLoader(SourceOrigin origin, std::span<const SourceInput> inputs, ModuleCache& cache)
      : origin_(origin), cache_(&cache) {
    for (const auto& input : inputs) {
      if (input.path.starts_with("std:")) {
        Error(SourceLocation(), std::string(input.path),
              "standard module paths cannot be overridden");
        continue;
      }
      const auto path = Normalize(Utf8Path(input.path));
      if (!path) {
        Error(SourceLocation(), std::string(input.path), path.error());
      } else if (!overlays_.emplace(PathText(*path), input.text).second) {
        Error(SourceLocation(), std::string(input.path), "duplicate module source path");
      }
    }
  }

  auto Run(std::string_view entry) -> ParseResult;
  auto RunPrepared(std::string_view entry, Program root, std::string source) -> ParseResult;

 private:
  [[nodiscard]] auto Normalize(const std::filesystem::path& path) const
      -> std::expected<std::filesystem::path, std::string>;
  auto Read(const std::filesystem::path& path, const std::string& key) const
      -> std::expected<std::string, std::string>;
  auto Load(const std::filesystem::path& file, SourceLocation location, const std::string& importer)
      -> std::optional<std::uint32_t>;
  void LoadDependencies(std::size_t index);
  auto LoadStandard(const Program::Import& imported, const std::string& importer)
      -> std::optional<std::uint32_t>;
  auto ParseModule(const std::string& key, std::string source) -> std::optional<std::uint32_t>;
  void Error(SourceLocation location, const std::string& path, std::string message);
  auto Finish() -> ParseResult;

  SourceOrigin origin_;
  absl::flat_hash_map<std::string, std::string_view> overlays_;
  absl::flat_hash_map<std::string, std::uint32_t> ids_;
  std::vector<SourceModule> modules_;
  std::vector<SourceFile> sources_;
  absl::flat_hash_map<std::string, std::string> resolutions_;
  Diagnostics diagnostics_;
  ModuleCache* cache_;
};

void ModuleLoader::Error(SourceLocation location, const std::string& path, std::string message) {
  diagnostics_.push_back(
      {.location = location, .message = std::move(message), .source_path = path});
}

auto ModuleLoader::Normalize(const std::filesystem::path& path) const
    -> std::expected<std::filesystem::path, std::string> {
  if (origin_ == SourceOrigin::Memory) {
    return path.lexically_normal();
  }
  std::error_code error;
  const auto requested = std::filesystem::absolute(path, error).lexically_normal();
  if (error) {
    return std::unexpected("cannot identify source path: " + error.message());
  }
  const auto requested_key = PathText(requested);
  if (const auto found = cache_->resolutions.find(requested_key);
      found != cache_->resolutions.end()) {
    return Utf8Path(found->second);
  }
  auto canonical = std::filesystem::weakly_canonical(path, error);
  if (error) {
    return std::unexpected("cannot resolve source path: " + error.message());
  }
  cache_->resolutions.emplace(requested_key, PathText(canonical));
  return canonical;
}

auto ModuleLoader::Read(const std::filesystem::path& path, const std::string& key) const
    -> std::expected<std::string, std::string> {
  const auto overlay = overlays_.find(key);
  if (overlay != overlays_.end()) {
    return std::string(overlay->second);
  }
  if (origin_ == SourceOrigin::Memory) {
    return std::unexpected("source module was not supplied: '" + key + "'");
  }
  if (const auto found = cache_->sources.find(key); found != cache_->sources.end()) {
    return *found->second;
  }
  const auto bytes = ReadBinaryFile(path);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  const void* storage = bytes->data();
  std::string source(static_cast<const char*>(storage), bytes->size());
  cache_->sources.emplace(key, std::make_shared<const std::string>(source));
  return source;
}

auto ModuleLoader::Load(const std::filesystem::path& file,
                        SourceLocation location,
                        const std::string& importer) -> std::optional<std::uint32_t> {
  const auto canonical = Normalize(file);
  if (!canonical) {
    Error(location, importer, canonical.error());
    return std::nullopt;
  }
  const auto key = PathText(*canonical);
  if (origin_ == SourceOrigin::Files) {
    std::error_code error;
    const auto requested = std::filesystem::absolute(file, error).lexically_normal();
    if (error) {
      Error(location, importer, "cannot identify requested source path: " + error.message());
      return std::nullopt;
    }
    const auto requested_key = PathText(requested);
    resolutions_.try_emplace(requested_key, key);
  }
  if (const auto found = ids_.find(key); found != ids_.end()) {
    return found->second;
  }
  auto source = Read(*canonical, key);
  if (!source) {
    Error(location, importer, source.error());
    return std::nullopt;
  }
  return ParseModule(key, std::move(*source));
}

auto ModuleLoader::ParseModule(const std::string& key, std::string source)
    -> std::optional<std::uint32_t> {
  const auto source_id = static_cast<std::uint32_t>(modules_.size());
  sources_.push_back({.path = key, .text = std::move(source)});
  auto parsed = ParseSource(sources_.back().text, source_id);
  if (!parsed.diagnostics.empty()) {
    for (auto& diagnostic : parsed.diagnostics) {
      diagnostic.source_path = key;
      diagnostics_.push_back(std::move(diagnostic));
    }
    return std::nullopt;
  }
  ids_.emplace(key, source_id);
  const auto binding =
      cache_->binding_ids.try_emplace(key, static_cast<std::uint32_t>(cache_->binding_ids.size()));
  modules_.push_back({
      .program = std::move(parsed.program),
      .dependencies = {},
      .binding_id = binding.first->second,
  });
  return source_id;
}

void ModuleLoader::LoadDependencies(std::size_t index) {
  const auto path = sources_.at(index).path;
  const auto importer = Utf8Path(path);
  auto imports = std::move(modules_.at(index).program.imports);
  std::vector<std::uint32_t> dependencies;
  for (const auto& imported : imports) {
    if (imported.path.starts_with("std:")) {
      const auto dependency = LoadStandard(imported, path);
      if (!dependency) {
        break;
      }
      dependencies.push_back(*dependency);
      continue;
    }
    const auto imported_path = Utf8Path(imported.path);
    if (imported.path.contains(':') && !imported_path.is_absolute()) {
      Error(imported.location, path, "unsupported module import scheme");
      break;
    }
    const auto dependency = Load(importer.parent_path() / imported_path, imported.location, path);
    if (!dependency) {
      break;
    }
    dependencies.push_back(*dependency);
  }
  // 加载会扩张模块向量；导入指令临时移出，避免跨扩容借用 AST 容器。
  modules_.at(index).program.imports = std::move(imports);
  modules_.at(index).dependencies = std::move(dependencies);
}

auto ModuleLoader::LoadStandard(const Program::Import& imported, const std::string& importer)
    -> std::optional<std::uint32_t> {
  const auto source = StandardModuleSource(imported.path);
  if (!source) {
    Error(imported.location, importer, "unknown standard module '" + imported.path + "'");
    return std::nullopt;
  }
  if (const auto found = ids_.find(imported.path); found != ids_.end()) {
    return found->second;
  }
  return ParseModule(imported.path, std::string(*source));
}

auto ModuleLoader::Run(std::string_view entry) -> ParseResult {
  if (diagnostics_.empty()) {
    const auto path = Utf8Path(entry);
    std::error_code error;
    if (origin_ == SourceOrigin::Files && std::filesystem::is_directory(path, error)) {
      Error(SourceLocation(), std::string(entry), "source path must name a file");
    }
    if (diagnostics_.empty()) {
      static_cast<void>(Load(path, SourceLocation(), std::string(entry)));
    }
  }
  return Finish();
}

auto ModuleLoader::RunPrepared(std::string_view entry, Program root, std::string source)
    -> ParseResult {
  const auto path = Normalize(Utf8Path(entry));
  if (!path) {
    Error(SourceLocation(), std::string(entry), path.error());
    return Finish();
  }
  const auto key = PathText(*path);
  ids_.emplace(key, 0);
  cache_->binding_ids.try_emplace(key, 0);
  sources_.push_back({.path = key, .text = std::move(source)});
  modules_.push_back({.program = std::move(root), .dependencies = {}, .binding_id = 0});
  return Finish();
}

auto ModuleLoader::Finish() -> ParseResult {
  for (std::size_t index = 0; index < modules_.size() && diagnostics_.empty(); ++index) {
    LoadDependencies(index);
  }
  if (!diagnostics_.empty()) {
    Program failed;
    failed.sources = std::move(sources_);
    return {.program = std::move(failed), .diagnostics = std::move(diagnostics_)};
  }
  auto result = BindModules(std::move(modules_), std::move(sources_));
  for (auto& [requested, canonical] : resolutions_) {
    result.program.file_resolutions.push_back(
        {.requested_path = requested, .canonical_path = std::move(canonical)});
  }
  return result;
}

}  // namespace

auto LoadProgram(std::string_view path, std::span<const SourceInput> overlays) -> ParseResult {
  ModuleCache cache;
  return ModuleLoader(SourceOrigin::Files, overlays, cache).Run(path);
}

auto LoadSources(std::span<const SourceInput> sources, std::string_view entry) -> ParseResult {
  ModuleCache cache;
  return ModuleLoader(SourceOrigin::Memory, sources, cache).Run(entry);
}

auto LoadPreparedProgram(std::string_view path,
                         Program root,
                         std::string source,
                         ModuleCache& cache) -> ParseResult {
  return ModuleLoader(SourceOrigin::Files, {}, cache)
      .RunPrepared(path, std::move(root), std::move(source));
}

}  // namespace vela::frontend
