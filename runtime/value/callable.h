#pragma once

#include <cstdint>
#include <optional>

#include "runtime/value/value.h"

namespace vela::runtime {

struct ResolvedCallable final {
  std::uint32_t function = 0;
  std::optional<Value> implicit = std::nullopt;
};

}  // namespace vela::runtime
