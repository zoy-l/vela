#pragma once

#include <cstdint>

namespace vela {

struct SourceLocation final {
  std::uint32_t line = 1;
  std::uint32_t column = 1;
  std::uint32_t source_id = 0;

  auto operator==(const SourceLocation&) const -> bool = default;
};

}  // namespace vela
