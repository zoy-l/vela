#pragma once

#include <expected>
#include <span>

#include "base/async_operation.h"
#include "base/execution_status.h"
#include "runtime/future.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;

[[nodiscard]] std::expected<Value, ExecutionStatus> ExecuteAsyncState(
    AsyncOperation operation,
    std::span<const Value> arguments,
    Isolate& isolate,
    const FutureDiagnostic& error);

}  // namespace vela::runtime
