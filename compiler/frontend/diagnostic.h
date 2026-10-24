#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vela::frontend {

struct SourceLocation final {
  std::uint32_t line = 1;
  std::uint32_t column = 1;
};

struct Diagnostic final {
  SourceLocation location;
  std::string message;
};

using Diagnostics = std::vector<Diagnostic>;

}  // namespace vela::frontend
