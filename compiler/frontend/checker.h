#pragma once

#include <string_view>

#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

struct CheckResult final {
  Diagnostics diagnostics;
  std::size_t function_count = 0;

  [[nodiscard]] bool Succeeded() const { return diagnostics.empty(); }
};

[[nodiscard]] CheckResult CheckSource(std::string_view source);

}  // namespace vela::frontend
