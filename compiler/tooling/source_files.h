#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace vela::tooling {

[[nodiscard]] std::expected<std::vector<std::filesystem::path>, std::string> DirectorySources(
    const std::filesystem::path& directory);

}  // namespace vela::tooling
