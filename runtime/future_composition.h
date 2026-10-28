#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <span>

#include "base/async_operation.h"
#include "base/execution_status.h"
#include "runtime/future.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;

// 后端仅提供调用能力；捕获的代码句柄不得永久保根程序状态或脚本值。
// managed 值由 FutureCall::retained 与组合操作的引用图负责追踪。
struct FutureCallbacks final {
  std::function<std::expected<std::size_t, ExecutionStatus>(Value)> arity;
  std::function<Future(Value, std::span<const Value>)> invoke;
};

struct FutureCall final {
  AsyncOperation operation = AsyncOperation::Count;
  std::span<const Value> values;
  std::optional<TypeConstraint> runtime_type = std::nullopt;
  const ArrayLayout* wait_layout = nullptr;
  std::span<const Value> retained;
  FutureDiagnostic type_error;
  FutureDiagnostic timeout_error;

  [[nodiscard]] Value At(std::size_t index) const { return values.subspan(index).front(); }
};

[[nodiscard]] std::expected<Value, ExecutionStatus>
ExecuteFutureOperation(Isolate& isolate, const FutureCall& call, const FutureCallbacks& callbacks);

}  // namespace vela::runtime
