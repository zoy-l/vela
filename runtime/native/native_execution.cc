#include "runtime/native/native_execution.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>

#include "base/execution_status.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/value/value.h"

namespace vela::runtime {
[[clang::always_inline]] NativeExecution::NativeExecution(NativeInstance& instance) noexcept
    : NativeExecution(instance, nullptr) {}

[[clang::always_inline]] NativeExecution::NativeExecution(NativeInstance& instance,
                                                          const NativeExecution* parent) noexcept
    : instance_(&instance),
      context_(instance.isolate(), *this),
      control_state_(&isolate().native_control().state()),
      barrier_state_(&isolate().heap().barrier_state()),
      stack_roots_(isolate().heap().native_stack_roots_head()),
      execution_resource_(isolate().external_resources(), ExternalResourceKind::Execution) {
  const auto limit = CurrentNativeStackLimit(*control_state_);
  if (parent != nullptr) {
    current_frame_ = parent->current_frame_;
    frame_count_ = parent->frame_count_;
    base_frame_count_ = frame_count_;
  }
  if (!limit) {
    stack_status_ = limit.error();
  } else {
    stack_limit_ = *limit;
    stack_status_ = ExecutionStatus::Returned;
  }
  if (instance.image().IsReloadable()) {
    isolate().native_dispatch().RegisterExecution(*this);
  }
}

NativeExecution::~NativeExecution() {
  if (version_previous_link_ != nullptr) {
    NativeDispatch::UnregisterExecution(*this);
  }
}

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

[[clang::noinline]] ExecutionResult CompleteFailure(NativeExecution& execution,
                                                    ExecutionStatus status) {
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

ExecutionResult Complete(NativeExecution& execution, std::expected<Value, ExecutionStatus> result) {
  if (result) [[likely]] {
    return {.value = *result, .stack_trace = {}, .message = {}, .origin = {}};
  }
  return CompleteFailure(execution, result.error());
}

}  // namespace

[[clang::always_inline]] ExecutionResult ExecuteNativeFunction(NativeInstance& instance,
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
