#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "base/execution_status.h"
#include "runtime/future.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;

[[nodiscard]] std::expected<Value, ExecutionStatus> AllocateFutureValue(Isolate& isolate,
                                                                        Value value);
[[nodiscard]] std::expected<Value, ExecutionStatus>
AllocateFutureError(Isolate& isolate, Value value, const FutureDiagnostic& diagnostic);
[[nodiscard]] std::expected<Value, ExecutionStatus> AllocateSleepFuture(Isolate& isolate,
                                                                        std::int64_t milliseconds);
void CompleteFuture(Isolate& isolate, Future& future, const Future& source);
void CompleteFutureError(Isolate& isolate,
                         Future& future,
                         Value payload,
                         const FutureDiagnostic& diagnostic);

void ObserveCheckedCompletion(Isolate& isolate,
                              Future target,
                              Future& source,
                              std::optional<TypeConstraint> type,
                              const FutureDiagnostic& diagnostic,
                              const std::shared_ptr<std::vector<Value>>& references);

// 验证在源 Future 完成时执行，所保留的程序状态仅通过可达 Future 图追踪。
[[nodiscard]] std::expected<Value, ExecutionStatus> CheckFutureCompletion(
    Isolate& isolate,
    Value source,
    std::optional<TypeConstraint> payload,
    const FutureDiagnostic& diagnostic,
    std::span<const Value> retained);

}  // namespace vela::runtime
