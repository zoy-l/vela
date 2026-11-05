#include "runtime/native/host_value.h"

#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include <vela/error.h>
#include <vela/handles.h>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_instance.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::internal {
Error CaptureHostError(runtime::Isolate& isolate,
                       ExecutionStatus status,
                       runtime::Value payload,
                       runtime::FutureDiagnostic diagnostic) {
  if (status == ExecutionStatus::OutOfMemory || status == ExecutionStatus::ExecutionStopped) {
    return {.status = status, .message = {}, .stack_trace = {}};
  }
  Error result{
      .status = status,
      .message = std::move(diagnostic.message),
      .stack_trace = std::move(diagnostic.stack_trace),
      .origin = diagnostic.origin,
  };
  if (!payload.IsNull()) {
    auto rooted = runtime::HostRoot::Create(isolate, payload);
    if (!rooted) {
      return {.status = rooted.error(), .message = {}, .stack_trace = {}};
    }
    HandleAccess::SetErrorPayload(result, std::move(*rooted));
  }
  return result;
}

Error CaptureHostError(runtime::NativeExecution& execution, ExecutionStatus status) {
  return CaptureHostError(execution.isolate(), status, execution.ExceptionPayload(),
                          execution.FailureDiagnostic(status));
}

void StoreHostResult(runtime::Heap& heap, runtime::Value& output, runtime::Value value) noexcept {
  output = value;
  heap.WriteRootBarrier(std::span(&output, 1));
}

std::string_view HostStringUtf8(runtime::Value value) noexcept {
  return value.AsString()->value();
}

Result<void> CreateHostString(HandleScope& scope, std::string_view text, runtime::Value& output) {
  const runtime::NativeContext context(scope.isolate());
  auto value = context.String(text);
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  StoreHostResult(scope.isolate().heap(), output, *value);
  return {};
}

Result<void> CastHostValue(HandleScope& scope,
                           runtime::Value value,
                           runtime::TypeConstraint type,
                           runtime::Value& output) {
  if (!runtime::Heap::IsValueOfType(value, type)) {
    return std::unexpected(
        Error{.status = ExecutionStatus::InvalidCast, .message = {}, .stack_trace = {}});
  }
  StoreHostResult(scope.isolate().heap(), output, value);
  return {};
}

Result<void> InvokeHostCallable(HandleScope& scope,
                                runtime::Value callable,
                                std::span<const runtime::Value> arguments,
                                runtime::Value& output) {
  auto& isolate = scope.isolate();
  const auto* parent = isolate.native_control().state().host_execution;
  if (parent != nullptr) {
    const auto resolved = runtime::ResolveNativeCallable(parent->instance(), callable);
    if (!resolved) {
      return std::unexpected(Error{.status = resolved.error(), .message = {}, .stack_trace = {}});
    }
  }
  auto* environment = runtime::Heap::NativeEnvironment(*callable.AsObject());
  auto instance = runtime::NativeInstance::Borrow(*environment);
  runtime::NativeExecution execution(instance, parent);
  const auto result = runtime::InvokeNativeCallable(execution, callable, arguments);
  if (!result) {
    return std::unexpected(CaptureHostError(execution, result.error()));
  }
  StoreHostResult(isolate.heap(), output, *result);
  return {};
}

}  // namespace vela::internal
