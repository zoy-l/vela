#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace vela::tooling {

[[nodiscard]] std::expected<std::filesystem::path, std::string> FileUriPath(std::string_view uri);
[[nodiscard]] std::string FileUri(const std::filesystem::path& path);

}  // namespace vela::tooling
