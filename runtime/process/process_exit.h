#pragma once

#include <expected>
#include <string_view>

#include "runtime/value/value.h"

namespace vela::runtime {

[[nodiscard]] std::expected<int, std::string_view> ProcessExitCode(Value value) noexcept;

}  // namespace vela::runtime
