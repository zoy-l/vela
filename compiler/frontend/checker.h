#pragma once

#include <cstddef>
#include <string_view>

#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

struct CheckResult final {
  Diagnostics diagnostics;
  std::size_t function_count = 0;

  [[nodiscard]] auto Succeeded() const -> bool { return diagnostics.empty(); }
};

[[nodiscard]] auto CheckSource(std::string_view source) -> CheckResult;
[[nodiscard]] auto CheckFile(std::string_view path) -> CheckResult;

}  // namespace vela::frontend
