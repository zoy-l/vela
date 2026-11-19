#pragma once

#include <expected>
#include <span>

#include "base/execution_status.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class NativeExecution;

}  // namespace vela::runtime

namespace vela::testing {

[[nodiscard]] std::expected<runtime::Value, ExecutionStatus> ExecuteNativeMock(
    runtime::NativeExecution& execution,
    std::span<const runtime::Value> arguments);

}  // namespace vela::testing
