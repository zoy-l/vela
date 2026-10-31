#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vela::tooling {

[[nodiscard]] std::optional<std::uint32_t> ParseConsoleIndex(std::string_view text);

}  // namespace vela::tooling
