#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace vela {

[[nodiscard]] auto Utf8Path(std::string_view text) -> std::filesystem::path;
[[nodiscard]] auto PathText(const std::filesystem::path& path) -> std::string;

}  // namespace vela
