#include "compiler/tooling/console_input.h"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace vela::tooling {

std::optional<std::uint32_t> ParseConsoleIndex(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint32_t index = 0;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto parsed = std::from_chars(begin, end, index);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return std::nullopt;
  }
  return index;
}

}  // namespace vela::tooling
