#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "compiler/project/project_config.h"

namespace vela::testing {

[[nodiscard]] std::expected<std::vector<std::filesystem::path>, compiler::ConfigurationError>
DiscoverSuites(const compiler::ProjectConfiguration& configuration,
               const std::filesystem::path& working_directory,
               std::span<const std::string> explicit_paths);

}  // namespace vela::testing
