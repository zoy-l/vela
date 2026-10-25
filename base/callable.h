#pragma once

#include <cstdint>
#include <limits>

namespace vela {

enum class CallableMode : std::uint8_t { Plain, Closure, BoundMethod, InterfaceMethod };

inline constexpr std::uint32_t kInvalidFunctionTarget = std::numeric_limits<std::uint32_t>::max();

struct CallableTarget final {
  std::uint32_t function = 0;
  CallableMode mode = CallableMode::Plain;
};

}  // namespace vela
