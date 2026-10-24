#pragma once

#include <string_view>

#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

struct CheckResult final {
  Diagnostics diagnostics;
  std::size_t function_count = 0;

  [[nodiscard]] auto Succeeded() const -> bool { return diagnostics.empty(); }
};

[[nodiscard]] auto CheckSource(std::string_view source) -> CheckResult;

}  // namespace vela::frontend
