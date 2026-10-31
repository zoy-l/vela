#pragma once

#include <expected>
#include <span>

#include "base/execution_status.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class NativeExecution;

[[nodiscard]] std::expected<Value, ExecutionStatus> ExecuteNativeMock(
    NativeExecution& execution,
    std::span<const Value> arguments);

}  // namespace vela::runtime
