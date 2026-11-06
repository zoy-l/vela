#pragma once

#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace vela {

[[nodiscard]] inline std::optional<std::uint32_t> ParseUint32(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint32_t value = 0;
  const auto characters = std::span(text);
  const auto* end = std::to_address(characters.end());
  const auto parsed = std::from_chars(characters.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  return value;
}

}  // namespace vela
