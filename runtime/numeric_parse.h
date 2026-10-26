#pragma once

#include <expected>
#include <span>

#include "base/builtin.h"
#include "runtime/builtins.h"
#include "runtime/value.h"

namespace vela::runtime {

[[nodiscard]] std::expected<Value, BuiltinError> ExecuteNumericParse(
    Builtin builtin,
    std::span<const Value> arguments);

}  // namespace vela::runtime
