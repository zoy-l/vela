#include "compiler/project/project_config.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <yyjson.h>

#include "base/file.h"
#include "base/json.h"
#include "base/path.h"
#include "base/process_exit.h"
#include "base/source_location.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/testing/pattern.h"
#include "compiler/testing/settings.h"

namespace vela::compiler {
namespace {

constexpr std::string_view kProjectConfigurationName = "vela.json";

bool IsSeparator(char value) {
  return value == ' ' || value == '\r' || value == '\n' || value == '\t' || value == ',' ||
         value == ':' || value == ']' || value == '}';
}

bool IsConcretePath(std::string_view text) {
  return !text.contains('\0') && text.find_first_of("\\*?[]{}") == std::string_view::npos;
}

std::size_t ConsumeToken(std::string_view source, std::size_t& cursor) {
  while (cursor < source.size() && IsSeparator(source.at(cursor))) {
    ++cursor;
  }
  const auto offset = cursor;
  const char token = source.at(cursor++);
  if (token == '"') {
    while (source.at(cursor) != '"') {
      if (source.at(cursor++) == '\\') {
        ++cursor;
      }
    }
    ++cursor;
  } else if (token != '{' && token != '[') {
    while (cursor < source.size() && !IsSeparator(source.at(cursor))) {
      ++cursor;
    }
  }
  return offset;
}

struct ConfigurationEntry final {
  const yyjson_val* value;
  std::string field;
};

std::vector<ConfigurationEntry> IndexChildren(const ConfigurationEntry& entry,
                                              std::optional<ConfigurationEntry>& duplicate) {
  std::vector<ConfigurationEntry> children;
  if (yyjson_is_obj(entry.value)) {
    absl::flat_hash_set<std::string_view> keys;
    auto iterator = yyjson_obj_iter_with(entry.value);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
      const auto name = GetJsonString(key);
      const auto field =
          entry.field.empty() ? std::string(name) : entry.field + "." + std::string(name);
      children.push_back({.value = key, .field = field});
      children.push_back({.value = yyjson_obj_iter_get_val(key), .field = field});
      if (!keys.insert(name).second && !duplicate) {
        duplicate = ConfigurationEntry{.value = key, .field = field};
      }
    }
  } else if (yyjson_is_arr(entry.value)) {
    auto iterator = yyjson_arr_iter_with(entry.value);
    std::size_t index = 0;
    while (const auto* value = yyjson_arr_iter_next(&iterator)) {
      children.push_back(
          {.value = value, .field = entry.field + "[" + std::to_string(index++) + "]"});
    }
  }
  return children;
}

class ConfigurationReader final {
 public:
  ConfigurationReader(const std::filesystem::path& file, std::string_view source)
      : file_(&file), source_(source) {}
  std::expected<void, ConfigurationError> Index(const yyjson_val* root);
  [[nodiscard]] ConfigurationError Error(const yyjson_val* value,
                                         std::string field,
                                         std::string message) const;
  [[nodiscard]] ConfigurationError Error(std::size_t offset,
                                         std::string field,
                                         std::string message) const;
  std::expected<void, ConfigurationError> Fields(const yyjson_val* object,
                                                 std::string_view field,
                                                 std::span<const std::string_view> allowed) const;
  std::expected<std::string, ConfigurationError> String(const yyjson_val* value,
                                                        std::string_view field) const;
  std::expected<testing::Timeout, ConfigurationError> Timeout(const yyjson_val* value,
                                                              std::string_view field) const;
  std::expected<testing::TagSelector, ConfigurationError> Selector(const yyjson_val* value,
                                                                   std::string_view field) const;

 private:
  [[nodiscard]] SourceLocation Location(std::size_t offset) const;
  const std::filesystem::path* file_;
  std::string_view source_;
  absl::flat_hash_map<const yyjson_val*, std::size_t> offsets_;
};

SourceLocation ConfigurationReader::Location(std::size_t offset) const {
  std::size_t line = 1;
  std::size_t column = 1;
  yyjson_locate_pos(source_.data(), source_.size(), offset, &line, &column, nullptr);
  return {.line = static_cast<std::uint32_t>(line), .column = static_cast<std::uint32_t>(column)};
}

ConfigurationError ConfigurationReader::Error(std::size_t offset,
                                              std::string field,
                                              std::string message) const {
  return {
      .file = *file_,
      .location = Location(offset),
      .field = std::move(field),
      .message = std::move(message),
      .exit_code = kUsageExitCode,
  };
}

ConfigurationError ConfigurationReader::Error(const yyjson_val* value,
                                              std::string field,
                                              std::string message) const {
  return Error(offsets_.at(value), std::move(field), std::move(message));
}

std::expected<void, ConfigurationError> ConfigurationReader::Index(const yyjson_val* root) {
  std::vector<ConfigurationEntry> pending{{.value = root, .field = {}}};
  std::optional<ConfigurationEntry> duplicate;
  std::size_t cursor = 0;
  while (!pending.empty()) {
    auto entry = std::move(pending.back());
    pending.pop_back();
    // yyjson 已验证完整语法；这里只按节点源码顺序记录 token 起点，不另写 JSON 解析器。
    offsets_.emplace(entry.value, ConsumeToken(source_, cursor));
    auto children = IndexChildren(entry, duplicate);
    for (auto& child : children | std::views::reverse) {
      pending.push_back(std::move(child));
    }
  }
  if (duplicate) {
    return std::unexpected(
        Error(duplicate->value, std::move(duplicate->field), "duplicate JSON key"));
  }
  return {};
}

std::expected<void, ConfigurationError> ConfigurationReader::Fields(
    const yyjson_val* object,
    std::string_view field,
    std::span<const std::string_view> allowed) const {
  if (!yyjson_is_obj(object)) {
    return std::unexpected(Error(object, std::string(field), "must be an object"));
  }
  auto iterator = yyjson_obj_iter_with(object);
  while (const auto* key = yyjson_obj_iter_next(&iterator)) {
    const auto name = GetJsonString(key);
    if (std::ranges::find(allowed, name) == allowed.end()) {
      return std::unexpected(Error(
          key, field.empty() ? std::string(name) : std::string(field) + "." + std::string(name),
          "unknown field"));
    }
  }
  return {};
}

std::expected<std::string, ConfigurationError> ConfigurationReader::String(
    const yyjson_val* value,
    std::string_view field) const {
  if (!yyjson_is_str(value) || yyjson_get_len(value) == 0) {
    return std::unexpected(Error(value, std::string(field), "must be a nonempty string"));
  }
  return std::string(GetJsonString(value));
}

std::expected<testing::Timeout, ConfigurationError> ConfigurationReader::Timeout(
    const yyjson_val* value,
    std::string_view field) const {
  const auto text = String(value, field);
  if (!text) {
    return std::unexpected(text.error());
  }
  const auto timeout = testing::ParseTimeout(*text);
  if (!timeout) {
    return std::unexpected(Error(value, std::string(field), timeout.error()));
  }
  return *timeout;
}

std::expected<testing::TagSelector, ConfigurationError> ConfigurationReader::Selector(
    const yyjson_val* value,
    std::string_view field) const {
  const auto text = String(value, field);
  if (!text) {
    return std::unexpected(text.error());
  }
  auto selector = testing::TagSelector::Parse(*text);
  if (!selector) {
    return std::unexpected(Error(value, std::string(field), selector.error()));
  }
  return std::move(*selector);
}

std::expected<testing::TagConfiguration, ConfigurationError> ReadTag(
    const ConfigurationReader& reader,
    yyjson_val* key) {
  const auto* value = yyjson_obj_iter_get_val(key);
  const auto name = GetJsonString(key);
  const auto field = "test.tags." + std::string(name);
  auto selector = reader.Selector(key, field);
  if (!selector) {
    return std::unexpected(selector.error());
  }
  constexpr auto kFields = std::to_array<std::string_view>({"timeout", "skip"});
  const auto fields = reader.Fields(value, field, kFields);
  if (!fields) {
    return std::unexpected(fields.error());
  }
  testing::TagConfiguration result{
      .selector = std::move(*selector),
      .timeout = std::nullopt,
      .skip = std::nullopt,
  };
  if (const auto* timeout = GetJsonField(value, "timeout")) {
    const auto parsed = reader.Timeout(timeout, field + ".timeout");
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    result.timeout = *parsed;
  }
  if (const auto* skip = GetJsonField(value, "skip")) {
    if (yyjson_is_str(skip)) {
      result.skip = testing::Skip{.enabled = true, .reason = std::string(GetJsonString(skip))};
    } else if (yyjson_is_true(skip)) {
      result.skip = testing::Skip{.enabled = true, .reason = "skipped by tag configuration"};
    } else if (yyjson_is_false(skip)) {
      result.skip = testing::Skip{.reason = {}};
    } else {
      return std::unexpected(reader.Error(skip, field + ".skip", "must be a boolean or string"));
    }
  }
  return result;
}

std::expected<void, ConfigurationError> ReadPaths(const ConfigurationReader& reader,
                                                  const yyjson_val* value,
                                                  testing::Settings& settings) {
  if (!yyjson_is_arr(value) || yyjson_get_len(value) == 0) {
    return std::unexpected(reader.Error(value, "test.paths", "must be a nonempty string array"));
  }
  auto iterator = yyjson_arr_iter_with(value);
  std::size_t index = 0;
  while (const auto* path = yyjson_arr_iter_next(&iterator)) {
    const auto field = "test.paths[" + std::to_string(index++) + "]";
    auto text = reader.String(path, field);
    if (!text) {
      return std::unexpected(text.error());
    }
    if (!IsConcretePath(*text)) {
      return std::unexpected(
          reader.Error(path, field, "must be a concrete path using / separators"));
    }
    settings.paths.push_back(std::move(*text));
  }
  return {};
}

std::expected<void, ConfigurationError> ReadTestDefaults(const ConfigurationReader& reader,
                                                         const yyjson_val* object,
                                                         testing::Settings& result) {
  if (const auto* paths = GetJsonField(object, "paths")) {
    const auto read = ReadPaths(reader, paths, result);
    if (!read) {
      return std::unexpected(read.error());
    }
  }
  if (const auto* filename = GetJsonField(object, "filename")) {
    auto text = reader.String(filename, "test.filename");
    if (!text) {
      return std::unexpected(text.error());
    }
    result.filename = std::move(*text);
    const auto pattern = testing::Pattern::Glob(result.filename);
    if (!pattern) {
      return std::unexpected(reader.Error(filename, "test.filename", pattern.error()));
    }
  }
  for (const auto* const name : {"timeout", "suite_load_timeout"}) {
    if (const auto* value = GetJsonField(object, name)) {
      const auto field = "test." + std::string(name);
      const auto timeout = reader.Timeout(value, field);
      if (!timeout) {
        return std::unexpected(timeout.error());
      }
      if (name == std::string_view("suite_load_timeout")) {
        if (timeout->kind == testing::TimeoutKind::Factor) {
          return std::unexpected(
              reader.Error(value, field, "requires an absolute duration or none"));
        }
        result.suite_load_timeout = *timeout;
      } else {
        result.timeout = *timeout;
      }
    }
  }
  return {};
}

std::expected<void, ConfigurationError> ReadRunnerSettings(const ConfigurationReader& reader,
                                                           const yyjson_val* object,
                                                           testing::Settings& result) {
  if (const auto* concurrency = GetJsonField(object, "concurrency")) {
    const auto count = yyjson_get_uint(concurrency);
    if (!yyjson_is_uint(concurrency) || count == 0 ||
        count > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(
          reader.Error(concurrency, "test.concurrency", "must be a positive integer"));
    }
    result.concurrency = static_cast<std::uint32_t>(count);
  }
  if (const auto* reporter = GetJsonField(object, "reporter")) {
    const auto text = reader.String(reporter, "test.reporter");
    if (!text) {
      return std::unexpected(text.error());
    }
    const auto parsed = testing::ParseReporter(*text);
    if (!parsed) {
      return std::unexpected(reader.Error(reporter, "test.reporter", parsed.error()));
    }
    result.reporter = *parsed;
  }
  for (const auto* const name : {"include_tags", "exclude_tags"}) {
    if (const auto* value = GetJsonField(object, name)) {
      auto selector = reader.Selector(value, "test." + std::string(name));
      if (!selector) {
        return std::unexpected(selector.error());
      }
      if (name == std::string_view("include_tags")) {
        result.include_tags = std::move(*selector);
      } else {
        result.exclude_tags = std::move(*selector);
      }
    }
  }
  return {};
}

std::expected<void, ConfigurationError> ReadTags(const ConfigurationReader& reader,
                                                 const yyjson_val* object,
                                                 testing::Settings& result) {
  if (const auto* tags = GetJsonField(object, "tags")) {
    if (!yyjson_is_obj(tags)) {
      return std::unexpected(reader.Error(tags, "test.tags", "must be an object"));
    }
    auto iterator = yyjson_obj_iter_with(tags);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
      auto tag = ReadTag(reader, key);
      if (!tag) {
        return std::unexpected(tag.error());
      }
      result.tags.push_back(std::move(*tag));
    }
  }
  return {};
}

std::expected<testing::Settings, ConfigurationError> ReadTestSettings(
    const ConfigurationReader& reader,
    const yyjson_val* object) {
  constexpr auto kFields = std::to_array<std::string_view>({
      "paths",
      "filename",
      "timeout",
      "suite_load_timeout",
      "concurrency",
      "reporter",
      "include_tags",
      "exclude_tags",
      "tags",
  });
  const auto fields = reader.Fields(object, "test", kFields);
  if (!fields) {
    return std::unexpected(fields.error());
  }
  testing::Settings result;
  const auto defaults = ReadTestDefaults(reader, object, result);
  if (!defaults) {
    return std::unexpected(defaults.error());
  }
  const auto runner = ReadRunnerSettings(reader, object, result);
  if (!runner) {
    return std::unexpected(runner.error());
  }
  const auto tags = ReadTags(reader, object, result);
  if (!tags) {
    return std::unexpected(tags.error());
  }
  return result;
}

std::expected<frontend::DiagnosticOptions, ConfigurationError> ReadDiagnosticOptions(
    const ConfigurationReader& reader,
    const yyjson_val* diagnostics) {
  frontend::DiagnosticOptions result;
  if (diagnostics == nullptr) {
    return result;
  }
  constexpr auto kDiagnostics = std::to_array<std::string_view>({"implicit_dynamic_conversion"});
  const auto fields = reader.Fields(diagnostics, "diagnostics", kDiagnostics);
  if (!fields) {
    return std::unexpected(fields.error());
  }
  if (const auto* value = GetJsonField(diagnostics, "implicit_dynamic_conversion")) {
    const auto severity = reader.String(value, "diagnostics.implicit_dynamic_conversion");
    if (!severity) {
      return std::unexpected(severity.error());
    }
    if (*severity != "warning" && *severity != "error") {
      return std::unexpected(reader.Error(value, "diagnostics.implicit_dynamic_conversion",
                                          "must be 'warning' or 'error'"));
    }
    result.implicit_dynamic_conversion = *severity == "error"
                                             ? frontend::DiagnosticSeverity::Error
                                             : frontend::DiagnosticSeverity::Warning;
  }
  return result;
}

}  // namespace

ConfigurationError ConfigurationError::FileError(const std::filesystem::path& file,
                                                 std::string message,
                                                 int exit_code) {
  return {
      .file = file,
      .location = {},
      .field = {},
      .message = std::move(message),
      .exit_code = exit_code,
  };
}

std::expected<ProjectConfiguration, ConfigurationError> ParseProjectConfiguration(
    const std::filesystem::path& file,
    std::string_view source) {
  ConfigurationReader reader(file, source);
  const auto document = JsonDocument::Parse(source);
  if (!document) {
    auto error = reader.Error(document.error().offset, {}, document.error().message);
    if (document.error().out_of_memory) {
      error.exit_code = kRuntimeErrorExitCode;
    }
    return std::unexpected(std::move(error));
  }
  const auto* root = document->root();
  const auto indexed = reader.Index(root);
  if (!indexed) {
    return std::unexpected(indexed.error());
  }
  constexpr auto kSections = std::to_array<std::string_view>({"test", "diagnostics", "imports"});
  const auto fields = reader.Fields(root, {}, kSections);
  if (!fields) {
    return std::unexpected(fields.error());
  }
  ProjectConfiguration result{
      .file = file.lexically_normal(),
      .root = file.parent_path(),
      .test = std::nullopt,
      .diagnostics = {},
      .imports = {},
  };
  const auto diagnostics = ReadDiagnosticOptions(reader, GetJsonField(root, "diagnostics"));
  if (!diagnostics) {
    return std::unexpected(diagnostics.error());
  }
  result.diagnostics = *diagnostics;
  if (const auto* imports = GetJsonField(root, "imports")) {
    if (!yyjson_is_obj(imports)) {
      return std::unexpected(reader.Error(imports, "imports", "must be an object"));
    }
    auto iterator = yyjson_obj_iter_with(imports);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
      const auto name = GetJsonString(key);
      if (name.empty() || name.starts_with("sdk:") || name.starts_with("builtin:") ||
          name.contains('\0')) {
        return std::unexpected(reader.Error(key, "imports", "invalid or reserved module name"));
      }
      auto value = reader.String(yyjson_obj_iter_get_val(key), "imports." + std::string(name));
      if (!value) {
        return std::unexpected(value.error());
      }
      if (value->contains('\0')) {
        return std::unexpected(
            reader.Error(key, "imports." + std::string(name), "module path contains a null byte"));
      }
      result.imports.emplace(name, PathText((result.root / Utf8Path(*value)).lexically_normal()));
    }
  }
  if (const auto* test = GetJsonField(root, "test")) {
    auto settings = ReadTestSettings(reader, test);
    if (!settings) {
      return std::unexpected(settings.error());
    }
    result.test = std::move(*settings);
  }
  return result;
}

namespace {

std::expected<ProjectConfiguration, ConfigurationError> ReadConfigurationFile(
    const std::filesystem::path& file) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(file, error);
  if (error) {
    return std::unexpected(ConfigurationError::FileError(file, error.message(), kIoErrorExitCode));
  }
  const auto bytes = ReadBinaryFile(canonical);
  if (!bytes) {
    return std::unexpected(
        ConfigurationError::FileError(canonical, bytes.error(), kIoErrorExitCode));
  }
  const void* storage = bytes->data();
  return ParseProjectConfiguration(canonical, {static_cast<const char*>(storage), bytes->size()});
}

using ConfigurationResult = std::expected<ProjectConfiguration, ConfigurationError>;
using DirectoryConfigurations = absl::flat_hash_map<std::filesystem::path,
                                                    std::shared_ptr<const ConfigurationResult>,
                                                    std::hash<std::filesystem::path>>;

std::expected<std::filesystem::path, ConfigurationError> AbsoluteConfigurationPath(
    const std::filesystem::path& path) {
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error).lexically_normal();
  if (error) {
    return std::unexpected(ConfigurationError::FileError(path, error.message(), kIoErrorExitCode));
  }
  return absolute;
}

std::shared_ptr<const ConfigurationResult> DiscoverConfiguration(
    std::filesystem::path& directory,
    DirectoryConfigurations* directories,
    std::vector<std::filesystem::path>& visited) {
  for (;;) {
    if (directories != nullptr) {
      if (const auto cached = directories->find(directory); cached != directories->end()) {
        return cached->second;
      }
      visited.push_back(directory);
    }
    const auto candidate = directory / Utf8Path(kProjectConfigurationName);
    std::error_code error;
    const auto status = std::filesystem::status(candidate, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      return std::make_shared<const ConfigurationResult>(std::unexpected(
          ConfigurationError::FileError(candidate, error.message(), kIoErrorExitCode)));
    }
    if (std::filesystem::exists(status)) {
      return std::make_shared<const ConfigurationResult>(ReadConfigurationFile(candidate));
    }
    const auto parent = directory.parent_path();
    if (parent == directory) {
      return std::make_shared<const ConfigurationResult>(ProjectConfiguration{
          .file = {},
          .root = {},
          .test = std::nullopt,
          .diagnostics = {},
          .imports = {},
      });
    }
    directory = parent;
  }
}

ConfigurationResult FindConfiguration(const std::filesystem::path& working_directory,
                                      ConfigurationRequirement requirement,
                                      DirectoryConfigurations* directories = nullptr) {
  const auto working = AbsoluteConfigurationPath(working_directory);
  if (!working) {
    return std::unexpected(working.error());
  }
  auto directory = *working;
  std::vector<std::filesystem::path> visited;
  const auto discovered = DiscoverConfiguration(directory, directories, visited);
  if (directories != nullptr) {
    for (const auto& path : visited) {
      directories->emplace(path, discovered);
    }
  }
  auto result = *discovered;
  if (result && result->file.empty()) {
    if (requirement == ConfigurationRequirement::Required) {
      return std::unexpected(ConfigurationError{
          .file = directory / Utf8Path(kProjectConfigurationName),
          .location = {},
          .field = {},
          .message = "vela.json was not found",
          .exit_code = kUsageExitCode,
      });
    }
    result->root = *working;
  }
  return result;
}

ConfigurationResult SourceConfiguration(const std::filesystem::path& source,
                                        const std::optional<std::filesystem::path>& explicit_file,
                                        DirectoryConfigurations* directories = nullptr) {
  const auto absolute = AbsoluteConfigurationPath(source);
  if (!absolute) {
    return std::unexpected(absolute.error());
  }
  const auto working = absolute->parent_path();
  return explicit_file
             ? ReadConfigurationFile(working / *explicit_file)
             : FindConfiguration(working, ConfigurationRequirement::Optional, directories);
}

}  // namespace

std::expected<ProjectConfiguration, ConfigurationError> LoadProjectConfiguration(
    const std::filesystem::path& working_directory,
    const std::optional<std::filesystem::path>& explicit_file,
    ConfigurationRequirement requirement) {
  return explicit_file ? ReadConfigurationFile(working_directory / *explicit_file)
                       : FindConfiguration(working_directory, requirement);
}

std::expected<ProjectConfiguration, ConfigurationError> LoadSourceConfiguration(
    const std::filesystem::path& source,
    const std::optional<std::filesystem::path>& explicit_file) {
  return SourceConfiguration(source, explicit_file);
}

std::expected<ProjectConfiguration, ConfigurationError> SourceConfigurationCache::Load(
    const std::filesystem::path& source) {
  return SourceConfiguration(source, std::nullopt, &directories_);
}

std::expected<ProjectConfiguration, ConfigurationError> ReadSourceConfiguration(
    const std::filesystem::path& source,
    std::ostream& errors,
    const std::optional<std::filesystem::path>& explicit_file) {
  auto configuration = LoadSourceConfiguration(source, explicit_file);
  if (!configuration) {
    frontend::WriteDiagnostic(errors, ConfigurationDiagnostic(configuration.error()));
  }
  return configuration;
}

frontend::Diagnostic ConfigurationDiagnostic(const ConfigurationError& error) {
  return {
      .location = error.location,
      .message = error.field.empty() ? error.message : error.field + ": " + error.message,
      .source_path = PathText(error.file),
      .kind = error.exit_code == kIoErrorExitCode ? frontend::DiagnosticKind::Io
                                                  : frontend::DiagnosticKind::Source,
  };
}

}  // namespace vela::compiler
