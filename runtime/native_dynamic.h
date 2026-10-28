#pragma once

#include <expected>
#include <span>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/native_abi.h"
#include "runtime/value.h"

namespace vela::runtime {

[[nodiscard]] std::expected<Value, ExecutionStatus> ExecuteNativeDynamicCall(
    NativeExecution& execution,
    Builtin builtin,
    std::span<const Value> arguments);

}  // namespace vela::runtime
