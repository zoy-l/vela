#pragma once

#include <expected>
#include <span>

#include "base/execution_status.h"
#include "runtime/future.h"
#include "runtime/future_composition.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;

[[nodiscard]] std::expected<Value, ExecutionStatus> ExecuteDynamicAsyncOperation(
    Isolate& isolate,
    FutureCall call,
    std::span<const TypeConstraint> types,
    const FutureCallbacks& callbacks,
    const FutureDiagnostic& completion_error);

}  // namespace vela::runtime
