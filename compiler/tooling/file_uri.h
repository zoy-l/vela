#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace vela::tooling {

[[nodiscard]] auto FileUriPath(std::string_view uri)
    -> std::expected<std::filesystem::path, std::string>;
[[nodiscard]] auto FileUri(const std::filesystem::path& path) -> std::string;

}  // namespace vela::tooling
