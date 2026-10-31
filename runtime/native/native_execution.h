#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "base/execution_status.h"
#include "runtime/async/execution_context.h"
#include "runtime/async/future.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class NativeInstance;

struct ExecutionResult final {
  // 返回值或异常载荷借用 Isolate 堆；宿主须在下一次分配前保根。
  Value value = Value::Null();
  ExecutionStatus status = ExecutionStatus::Returned;
  std::vector<FutureTraceFrame> stack_trace;
  std::string message;
  ErrorOrigin origin;

  [[nodiscard]] bool Succeeded() const noexcept { return status == ExecutionStatus::Returned; }
};

[[nodiscard]] ExecutionResult ExecuteNativeFunction(NativeInstance& instance,
                                                    std::uint32_t function,
                                                    std::span<const Value> arguments = {});
[[nodiscard]] ExecutionResult ExecuteNativeCallable(NativeInstance& instance,
                                                    Value callable,
                                                    std::span<const Value> arguments = {});
[[nodiscard]] ExecutionResult FutureFailure(const FutureResult& completion);

}  // namespace vela::runtime
