#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace vela {

[[nodiscard]] std::filesystem::path Utf8Path(std::string_view text);
[[nodiscard]] std::string PathText(const std::filesystem::path& path);

}  // namespace vela
