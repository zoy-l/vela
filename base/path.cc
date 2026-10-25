#include "base/path.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace vela {

auto Utf8Path(std::string_view text) -> std::filesystem::path {
  return {std::u8string(text.begin(), text.end())};
}

auto PathText(const std::filesystem::path& path) -> std::string {
  const auto encoded = path.generic_u8string();
  return {encoded.begin(), encoded.end()};
}

}  // namespace vela
