#include "base/command_line.h"
#include <filesystem>
#include <optional>
#include <string_view>

#include "base/path.h"

namespace vela {

std::optional<std::filesystem::path> ReadPathOption(std::string_view argument,
                                                    std::string_view prefix) {
  if (!argument.starts_with(prefix) || argument.size() == prefix.size()) {
    return std::nullopt;
  }
  return Utf8Path(argument.substr(prefix.size()));
}

}  // namespace vela
