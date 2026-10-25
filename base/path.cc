#include "base/path.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace vela {

std::filesystem::path Utf8Path(std::string_view text) {
  return {std::u8string(text.begin(), text.end())};
}

std::string PathText(const std::filesystem::path& path) {
  const auto encoded = path.generic_u8string();
  return {encoded.begin(), encoded.end()};
}

}  // namespace vela
