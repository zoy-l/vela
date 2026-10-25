#pragma once

#include <cstdint>

namespace vela::runtime {

inline constexpr std::uint32_t kErrorFieldCount = 2;
inline constexpr std::uint32_t kErrorMessageField = 0;
inline constexpr std::uint32_t kErrorCodeField = 1;

}  // namespace vela::runtime
