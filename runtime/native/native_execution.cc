#include "runtime/native/native_execution.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>

#include "base/execution_status.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

ExecutionResult Failure(Value value, ExecutionStatus status, FutureDiagnostic diagnostic) {
  return {
      .value = value,
      .status = status,
      .stack_trace = std::move(diagnostic.stack_trace),
      .message = std::move(diagnostic.message),
      .origin = diagnostic.origin,
  };
}

ExecutionResult Complete(NativeExecution& execution, std::expected<Value, ExecutionStatus> result) {
  if (result) {
    return {.value = *result, .stack_trace = {}, .message = {}, .origin = {}};
  }
  const auto status = result.error();
  if (!execution.HasException()) {
    return {
        .status = status,
        .stack_trace = {},
        .message = std::string(ExecutionStatusName(status)),
        .origin = {},
    };
  }
  return Failure(execution.ExceptionPayload(), status, execution.ExceptionDiagnostic());
}

}  // namespace

ExecutionResult ExecuteNativeFunction(NativeInstance& instance,
                                      std::uint32_t function,
                                      std::span<const Value> arguments) {
  NativeExecution execution(instance);
  return Complete(execution, InvokeNativeFunction(execution, function, arguments));
}

ExecutionResult ExecuteNativeCallable(NativeInstance& instance,
                                      Value callable,
                                      std::span<const Value> arguments) {
  NativeExecution execution(instance);
  return Complete(execution, InvokeNativeCallable(execution, callable, arguments));
}

ExecutionResult FutureFailure(const FutureResult& completion) {
  const auto& diagnostic = completion.diagnostic;
  return Failure(completion.value, diagnostic.status, diagnostic);
}

}  // namespace vela::runtime
