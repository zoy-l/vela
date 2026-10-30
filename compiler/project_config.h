#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "base/source_location.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/testing/settings.h"

namespace vela::compiler {

struct ConfigurationError final {
  std::filesystem::path file;
  SourceLocation location;
  std::string field;
  std::string message;
  int exit_code = 0;
};

// 命令会话拥有快照，并以 const 发布给消费者；读取后不再观察磁盘变化。
struct ProjectConfiguration final {
  std::filesystem::path file;
  std::filesystem::path root;
  std::optional<testing::Settings> test;
  frontend::DiagnosticOptions diagnostics;
};

enum class ConfigurationRequirement : std::uint8_t { Required, Optional };

[[nodiscard]] std::expected<ProjectConfiguration, ConfigurationError> ParseProjectConfiguration(
    const std::filesystem::path& file,
    std::string_view source);
[[nodiscard]] std::expected<ProjectConfiguration, ConfigurationError> LoadProjectConfiguration(
    const std::filesystem::path& working_directory,
    const std::optional<std::filesystem::path>& explicit_file = std::nullopt,
    ConfigurationRequirement requirement = ConfigurationRequirement::Required);

[[nodiscard]] std::expected<frontend::DiagnosticOptions, ConfigurationError>
LoadSourceDiagnosticOptions(const std::filesystem::path& source);
[[nodiscard]] frontend::Diagnostic ConfigurationDiagnostic(const ConfigurationError& error);

}  // namespace vela::compiler
