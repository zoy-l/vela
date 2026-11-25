#include "runtime/native/host_value.h"

#include <cassert>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include <vela/error.h>
#include <vela/handles.h>

#include "base/execution_status.h"
#include "runtime/heap/field_storage.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_callback.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::internal {
Error CaptureHostError(runtime::Isolate& isolate,
                       ExecutionStatus status,
                       runtime::Value payload,
                       runtime::FutureDiagnostic diagnostic) {
  if (status == ExecutionStatus::OutOfMemory || status == ExecutionStatus::ExecutionStopped) {
    return Error::FromStatus(status);
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
      return Error::FromStatus(rooted.error());
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
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidCast));
  }
  StoreHostResult(scope.isolate().heap(), output, value);
  return {};
}

const std::byte* InlineScalarOperand(const runtime::HeapObject& object,
                                     const runtime::NativeFunctionDescription& function) {
  const auto index = function.scalar_capture;
  if (index == runtime::kNoNativeIndex) {
    return std::as_bytes(std::span(&function.scalar_operand, 1)).data();
  }
  if ((function.flags & runtime::kNativeScalarCallbackReadsCell) != 0) {
    const auto& cell = *runtime::Heap::LoadField(object, index).AsObject();
    assert(runtime::Heap::ClassOf(cell)->storage() == runtime::ClassStorage::Inline);
    return runtime::ObjectFieldStorage(cell).data();
  }
  assert(runtime::Heap::ClassOf(object)->storage() == runtime::ClassStorage::Inline);
  const auto fields = runtime::ObjectFieldStorage(object);
  return fields.subspan(static_cast<std::size_t>(index) * runtime::FieldStorage::kPayloadBytes)
      .data();
}

BoundScalarCallback BoundScalarCallback::Bind(runtime::Isolate& isolate,
                                              runtime::Value callable) noexcept {
  const auto& object = *callable.AsObject();
  const auto* function = runtime::Heap::CallableFunction(object);
  auto result = BoundScalarCallback{.control = &isolate.native_control().state()};
  if (function == nullptr) {
    return result;
  }
  result.entry = function->integer_callback;
  if ((function->flags & runtime::kNativeInlineScalarCallback) != 0) {
    const auto& control = *result.control;
    result.operand = InlineScalarOperand(object, *function);
    result.requests = control.requests;
    result.stack_limit = control.callback_stack_limit;
  }
  return result;
}

Result<void> InvokeHostCallable(runtime::Isolate& isolate,
                                runtime::Value callable,
                                std::span<const runtime::Value> arguments,
                                runtime::Value& output) {
  const auto* parent = isolate.native_control().state().host_execution;
  auto* environment = runtime::Heap::NativeEnvironment(*callable.AsObject());
  auto instance = runtime::NativeInstance::Borrow(*environment);
  const auto& domain = parent != nullptr ? parent->instance() : instance;
  const auto resolved = runtime::ResolveNativeCallable(domain, callable);
  if (!resolved) {
    return std::unexpected(Error::FromStatus(resolved.error()));
  }
  runtime::NativeExecution execution(instance, parent);
  // 纯 AOT 回调可能没有内部安全点；宿主入口仍必须先处理已排队的请求。
  const auto ready = isolate.native_control().ProcessSafepoint(&execution);
  if (ready != ExecutionStatus::Returned) {
    return std::unexpected(Error::FromStatus(ready));
  }
  const auto result = runtime::InvokeResolvedNativeCallable(execution, *resolved, arguments);
  if (!result) {
    return std::unexpected(CaptureHostError(execution, result.error()));
  }
  StoreHostResult(isolate.heap(), output, *result);
  return {};
}

}  // namespace vela::internal
