#pragma once

#include <expected>
#include <span>

#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/async/future_composition.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Isolate;

[[nodiscard]] std::expected<Value, ExecutionStatus> ExecuteDynamicAsyncOperation(
    Isolate& isolate,
    FutureCall call,
    std::span<const TypeConstraint> types,
    const FutureCallbacks& callbacks,
    const FutureDiagnostic& completion_error);

}  // namespace vela::runtime
