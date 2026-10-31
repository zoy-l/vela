#include "compiler/frontend/modules/source_loader.h"

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
#include <absl/container/flat_hash_set.h>

#include "base/file.h"
#include "base/path.h"
#include "compiler/frontend/builtins/sdk_modules.h"
#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/module_binding.h"
#include "compiler/frontend/modules/source_file.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/ast_clone.h"
#include "compiler/frontend/syntax/parser.h"

namespace vela::frontend {
namespace {

enum class SourceOrigin : std::uint8_t { Files, Memory };
// 单次加载直接转移 AST；跨根或会话缓存保留未绑定模板，每个绑定上下文独立克隆。
enum class SyntaxRetention : std::uint8_t { Transfer, Cache };

struct ResolvedPath final {
  std::filesystem::path canonical;
  std::string requested_key;
};

class ModuleLoader final {
 public:
  ModuleLoader(SourceOrigin origin,
               std::span<const SourceInput> inputs,
               ModuleCache& cache,
               SyntaxRetention retention,
               SourceLoadMode mode = SourceLoadMode::Compilation)
      : origin_(origin), mode_(mode), retention_(retention), cache_(&cache) {
    for (const auto& input : inputs) {
      if (input.path.starts_with(kStandardSourcePrefix) ||
          input.path.starts_with(kSdkSourcePrefix)) {
        Error(SourceLocation(), std::string(input.path), "embedded sources cannot be overridden");
        continue;
      }
      const auto path = Normalize(Utf8Path(input.path));
      if (!path) {
        Error(SourceLocation(), std::string(input.path), path.error(), DiagnosticKind::Io);
      } else if (!overlays_.emplace(PathText(path->canonical), input.text).second) {
        Error(SourceLocation(), std::string(input.path), "duplicate module source path");
      }
    }
  }

  ModuleGraph Run(std::string_view entry);
  ModuleGraph Run(std::span<const std::string> roots);
  ModuleGraph RunPrepared(std::string_view entry, ParsedSource source);

 private:
  [[nodiscard]] std::expected<ResolvedPath, std::string> Normalize(
      const std::filesystem::path& path) const;
  [[nodiscard]] std::expected<ParsedSource, std::string> Read(const std::filesystem::path& path,
                                                              const std::string& key,
                                                              std::uint32_t source_id) const;
  std::optional<std::uint32_t> Load(const std::filesystem::path& file,
                                    SourceLocation location,
                                    const std::string& importer);
  std::optional<std::uint32_t> LoadSdk(std::string_view identity,
                                       SourceLocation location,
                                       const std::string& importer);
  void LoadDependencies(std::size_t index);
  std::optional<std::uint32_t> ParseModule(const std::string& key, ParsedSource source);
  void Error(SourceLocation location,
             const std::string& path,
             std::string message,
             DiagnosticKind kind = DiagnosticKind::Source);
  ModuleGraph Finish();

  SourceOrigin origin_;
  SourceLoadMode mode_;
  SyntaxRetention retention_;
  absl::flat_hash_map<std::string, std::string_view> overlays_;
  absl::flat_hash_map<std::string, std::uint32_t> ids_;
  std::vector<SourceModule> modules_;
  std::vector<SourceFile> sources_;
  absl::flat_hash_map<std::string, std::string> resolutions_;
  Diagnostics diagnostics_;
  Diagnostics syntax_diagnostics_;
  ModuleCache* cache_;
};

void ModuleLoader::Error(SourceLocation location,
                         const std::string& path,
                         std::string message,
                         DiagnosticKind kind) {
  diagnostics_.push_back(
      {.location = location, .message = std::move(message), .source_path = path, .kind = kind});
}

std::expected<ResolvedPath, std::string> ModuleLoader::Normalize(
    const std::filesystem::path& path) const {
  if (origin_ == SourceOrigin::Memory) {
    return ResolvedPath{.canonical = path.lexically_normal(), .requested_key = {}};
  }
  std::error_code error;
  const auto requested = std::filesystem::absolute(path, error);
  if (error) {
    return std::unexpected("cannot identify source path: " + error.message());
  }
  auto requested_key = PathText(requested.lexically_normal());
  if (const auto found = cache_->resolutions.find(requested_key);
      found != cache_->resolutions.end()) {
    return ResolvedPath{
        .canonical = Utf8Path(found->second),
        .requested_key = std::move(requested_key),
    };
  }
  auto canonical = std::filesystem::weakly_canonical(requested, error);
  if (error) {
    return std::unexpected("cannot resolve source path: " + error.message());
  }
  cache_->resolutions.emplace(requested_key, PathText(canonical));
  return ResolvedPath{.canonical = std::move(canonical), .requested_key = std::move(requested_key)};
}

std::expected<ParsedSource, std::string> ModuleLoader::Read(const std::filesystem::path& path,
                                                            const std::string& key,
                                                            std::uint32_t source_id) const {
  const auto instantiate = [source_id](const ParsedSource& source) -> ParsedSource {
    return {
        .source = source.source,
        .program = CloneProgram(source.program, {.source_id = source_id}),
        .diagnostics = source.diagnostics,
    };
  };
  const auto overlay = overlays_.find(key);
  const auto cached = cache_->sources.find(key);
  if (cached != cache_->sources.end() &&
      (origin_ == SourceOrigin::Files || overlay != overlays_.end()) &&
      (overlay == overlays_.end() || cached->second->source->text == overlay->second)) {
    return instantiate(*cached->second);
  }
  std::string text;
  if (overlay != overlays_.end()) {
    text = overlay->second;
  } else {
    if (origin_ == SourceOrigin::Memory) {
      return std::unexpected("source module was not supplied: '" + key + "'");
    }
    const auto bytes = ReadBinaryFile(path);
    if (!bytes) {
      return std::unexpected(bytes.error());
    }
    const void* storage = bytes->data();
    text.assign(static_cast<const char*>(storage), bytes->size());
  }
  if (retention_ == SyntaxRetention::Transfer) {
    return ParseSourceSnapshot(std::move(text), source_id);
  }
  auto parsed = std::make_shared<const ParsedSource>(ParseSourceSnapshot(std::move(text)));
  cache_->sources.insert_or_assign(key, parsed);
  return instantiate(*parsed);
}

std::optional<std::uint32_t> ModuleLoader::Load(const std::filesystem::path& file,
                                                SourceLocation location,
                                                const std::string& importer) {
  const auto supplied = PathText(file);
  if (supplied.starts_with(kSdkSourcePrefix)) {
    return LoadSdk(supplied, location, importer);
  }
  auto resolved = Normalize(file);
  if (!resolved) {
    Error(location, importer, resolved.error(), DiagnosticKind::Io);
    return std::nullopt;
  }
  const auto key = PathText(resolved->canonical);
  if (origin_ == SourceOrigin::Files) {
    resolutions_.try_emplace(std::move(resolved->requested_key), key);
  }
  if (const auto found = ids_.find(key); found != ids_.end()) {
    return found->second;
  }
  auto source = Read(resolved->canonical, key, static_cast<std::uint32_t>(modules_.size()));
  if (!source) {
    Error(location, importer, source.error(),
          origin_ == SourceOrigin::Files ? DiagnosticKind::Io : DiagnosticKind::Source);
    return std::nullopt;
  }
  return ParseModule(key, std::move(*source));
}

std::optional<std::uint32_t> ModuleLoader::ParseModule(const std::string& key,
                                                       ParsedSource source) {
  const auto source_id = static_cast<std::uint32_t>(modules_.size());
  const auto& snapshot = source.source;
  sources_.push_back({.path = key, .text = snapshot->text, .syntax = snapshot});
  if (!source.diagnostics.empty()) {
    auto& diagnostics = mode_ == SourceLoadMode::Editor ? syntax_diagnostics_ : diagnostics_;
    for (auto diagnostic : source.diagnostics) {
      diagnostic.location.source_id = source_id;
      diagnostic.source_path = key;
      diagnostics.push_back(std::move(diagnostic));
    }
    if (mode_ == SourceLoadMode::Compilation) {
      return std::nullopt;
    }
  }
  ids_.emplace(key, source_id);
  const auto binding =
      cache_->binding_ids.try_emplace(key, static_cast<std::uint32_t>(cache_->binding_ids.size()));
  modules_.push_back({
      .program = std::move(source.program),
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
    if (imported.path.starts_with(kSdkSourcePrefix)) {
      const auto dependency = LoadSdk(imported.path, imported.location, path);
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
    const auto dependency =
        Load(ResolveImportPath(importer, imported_path), imported.location, path);
    if (!dependency) {
      break;
    }
    dependencies.push_back(*dependency);
  }
  // 加载会扩张模块向量；导入指令临时移出，避免跨扩容借用 AST 容器。
  modules_.at(index).program.imports = std::move(imports);
  modules_.at(index).dependencies = std::move(dependencies);
}

std::optional<std::uint32_t> ModuleLoader::LoadSdk(std::string_view identity,
                                                   SourceLocation location,
                                                   const std::string& importer) {
  const std::string key(identity);
  if (const auto found = ids_.find(key); found != ids_.end()) {
    return found->second;
  }
  const auto text = SdkModuleSource(identity);
  if (!text) {
    Error(location, importer, "unknown SDK module '" + key + "'");
    return std::nullopt;
  }
  const auto cached = cache_->sources.find(key);
  std::shared_ptr<const ParsedSource> source;
  if (cached == cache_->sources.end()) {
    auto parsed = ParseSourceSnapshot(std::string(*text));
    BindSdkDeclarations(identity, parsed.program);
    source = std::make_shared<const ParsedSource>(std::move(parsed));
    cache_->sources.emplace(key, source);
  } else {
    source = cached->second;
  }
  const auto source_id = static_cast<std::uint32_t>(modules_.size());
  return ParseModule(key, {
                              .source = source->source,
                              .program = CloneProgram(source->program, {.source_id = source_id}),
                              .diagnostics = source->diagnostics,
                          });
}

ModuleGraph ModuleLoader::Run(std::string_view entry) {
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

ModuleGraph ModuleLoader::Run(std::span<const std::string> roots) {
  for (const auto& root : roots) {
    static_cast<void>(Load(Utf8Path(root), SourceLocation(), root));
  }
  return Finish();
}

ModuleGraph ModuleLoader::RunPrepared(std::string_view entry, ParsedSource source) {
  const auto path = Normalize(Utf8Path(entry));
  if (!path) {
    Error(SourceLocation(), std::string(entry), path.error(), DiagnosticKind::Io);
    return Finish();
  }
  const auto key = PathText(path->canonical);
  ids_.emplace(key, 0);
  const auto binding =
      cache_->binding_ids.try_emplace(key, static_cast<std::uint32_t>(cache_->binding_ids.size()));
  const auto& snapshot = source.source;
  sources_.push_back({.path = key, .text = snapshot->text, .syntax = snapshot});
  modules_.push_back({
      .program = std::move(source.program),
      .dependencies = {},
      .binding_id = binding.first->second,
  });
  return Finish();
}

ModuleGraph ModuleLoader::Finish() {
  for (std::size_t index = 0; index < modules_.size() && diagnostics_.empty(); ++index) {
    LoadDependencies(index);
  }
  std::vector<FileResolution> resolutions;
  resolutions.reserve(resolutions_.size());
  for (auto& [requested, canonical] : resolutions_) {
    resolutions.push_back({.requested_path = requested, .canonical_path = std::move(canonical)});
  }
  return {
      .modules = std::move(modules_),
      .sources = std::move(sources_),
      .file_resolutions = std::move(resolutions),
      .diagnostics = std::move(diagnostics_),
      .syntax_diagnostics = std::move(syntax_diagnostics_),
  };
}

}  // namespace

ModuleGraph LoadModuleGraph(std::string_view path) {
  ModuleCache cache;
  return ModuleLoader(SourceOrigin::Files, {}, cache, SyntaxRetention::Transfer).Run(path);
}

ModuleGraph LoadModuleGraph(std::span<const std::string> roots,
                            ModuleCache& cache,
                            std::span<const SourceInput> overlays) {
  return ModuleLoader(SourceOrigin::Files, overlays, cache, SyntaxRetention::Cache).Run(roots);
}

ParseResult BindModuleGraph(ModuleGraph graph,
                            std::span<const std::string> retained_standard_names) {
  if (!graph.diagnostics.empty()) {
    Program failed;
    failed.sources = std::move(graph.sources);
    return {.program = std::move(failed), .diagnostics = std::move(graph.diagnostics)};
  }
  auto result = BindModules(std::move(graph.modules), std::move(graph.sources));
  auto& standard_names = result.program.standard_names;
  absl::flat_hash_set<std::string> included(standard_names.begin(), standard_names.end());
  for (const auto& name : retained_standard_names) {
    if (included.insert(name).second) {
      standard_names.push_back(name);
    }
  }
  for (auto& diagnostic : AddStandardEnvironment(result.program)) {
    result.diagnostics.push_back(std::move(diagnostic));
  }
  for (auto& diagnostic : graph.syntax_diagnostics) {
    result.diagnostics.push_back(std::move(diagnostic));
  }
  result.program.file_resolutions = std::move(graph.file_resolutions);
  return result;
}

std::filesystem::path ResolveImportPath(const std::filesystem::path& importer,
                                        std::filesystem::path imported) {
  if (!imported.has_extension()) {
    imported.replace_extension(".vela");
  }
  return (importer.parent_path() / imported).lexically_normal();
}

ParseResult LoadProgram(std::string_view path,
                        std::span<const SourceInput> overlays,
                        SourceLoadMode mode) {
  ModuleCache cache;
  return BindModuleGraph(
      ModuleLoader(SourceOrigin::Files, overlays, cache, SyntaxRetention::Transfer, mode)
          .Run(path));
}

ParseResult LoadProgram(std::string_view path,
                        ModuleCache& cache,
                        std::span<const SourceInput> overlays,
                        SourceLoadMode mode) {
  return BindModuleGraph(
      ModuleLoader(SourceOrigin::Files, overlays, cache, SyntaxRetention::Cache, mode).Run(path));
}

ParseResult LoadSources(std::span<const SourceInput> sources, std::string_view entry) {
  ModuleCache cache;
  return BindModuleGraph(
      ModuleLoader(SourceOrigin::Memory, sources, cache, SyntaxRetention::Transfer).Run(entry));
}

ParseResult LoadSources(std::span<const SourceInput> sources,
                        std::span<const std::string> roots,
                        std::span<const std::string> retained_standard_names) {
  ModuleCache cache;
  return BindModuleGraph(
      ModuleLoader(SourceOrigin::Memory, sources, cache, SyntaxRetention::Transfer).Run(roots),
      retained_standard_names);
}

ParseResult LoadSources(std::span<const SourceInput> sources,
                        std::span<const std::string> roots,
                        ModuleCache& cache,
                        std::span<const std::string> retained_standard_names) {
  return BindModuleGraph(
      ModuleLoader(SourceOrigin::Memory, sources, cache, SyntaxRetention::Cache).Run(roots),
      retained_standard_names);
}

ParseResult LoadPreparedProgram(std::string_view path, ParsedSource source, ModuleCache& cache) {
  return BindModuleGraph(ModuleLoader(SourceOrigin::Files, {}, cache, SyntaxRetention::Cache)
                             .RunPrepared(path, std::move(source)));
}

}  // namespace vela::frontend
