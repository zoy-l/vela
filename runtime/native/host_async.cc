#include "runtime/native/host_async.h"
#include <optional>

#include <expected>
#include <utility>

#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/async/future_operations.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_value.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"
#include "vela/error.h"
#include "vela/handles.h"
#include "vela/types.h"

namespace vela::internal {
namespace {

Result<void> PublishFuture(HandleScope& scope,
                           runtime::HeapObject* object,
                           runtime::TypeConstraint type,
                           runtime::Value& output) {
  if (object == nullptr) {
    return std::unexpected(
        Error{.status = ExecutionStatus::OutOfMemory, .message = {}, .stack_trace = {}});
  }
  const auto bound = runtime::Heap::BindRuntimeType(*object, type);
  if (!bound) {
    return std::unexpected(Error{.status = bound.error(), .message = {}, .stack_trace = {}});
  }
  StoreHostResult(scope.isolate().heap(), output, runtime::Value::Object(object));
  return {};
}

}  // namespace

runtime::FutureState HostFutureState(runtime::Value future) noexcept {
  return runtime::Heap::GetFuture(*future.AsObject())->state();
}

Result<void> CompletionResult(std::expected<void, runtime::FutureCompletionError> result) {
  if (!result) {
    return std::unexpected(Error{
        .status = runtime::FutureCompletionStatus(result.error()),
        .message = {},
        .stack_trace = {},
    });
  }
  return {};
}

Result<void> CreateHostCompleter(HandleScope& scope,
                                 runtime::TypeConstraint type,
                                 runtime::Value& output) {
  auto& isolate = scope.isolate();
  runtime::Future future(&isolate.scheduler());
  return PublishFuture(scope, isolate.heap().AllocateCompleter(std::move(future)), type, output);
}

Result<runtime::FutureCompletionEndpoint> CreateHostCompletion(HandleScope& scope,
                                                               runtime::TypeConstraint type,
                                                               runtime::Value& output) {
  auto& isolate = scope.isolate();
  auto& heap = isolate.heap();
  auto completer = isolate.scheduler().CreateHostCompleter();
  auto raw = runtime::AllocatedValue(heap.AllocateFuture(completer.future()));
  if (!raw) {
    return std::unexpected(Error{.status = raw.error(), .message = {}, .stack_trace = {}});
  }
  // 源 Future 和接收端在同一托管图内；完成校验不以外部 Persistent 保活自身。
  const runtime::RootRange source_root(heap, &*raw, 1, std::nullopt);
  const auto checked = runtime::BindFutureCompletion(
      isolate, *raw, type,
      {
          .message = "host completion does not match its Future payload",
          .stack_trace = {},
          .status = ExecutionStatus::TypeError,
          .origin = {},
      });
  if (!checked) {
    return std::unexpected(Error{.status = checked.error(), .message = {}, .stack_trace = {}});
  }
  StoreHostResult(heap, output, *checked);
  return std::move(completer).Detach();
}

Result<void> ReadHostFuture(HandleScope& scope, runtime::Value future, runtime::Value& output) {
  auto& source = *runtime::Heap::GetFuture(*future.AsObject());
  source.MarkObserved();
  return ReadHostCompletion(scope, source.result(), output);
}

Result<void> ReadHostCompletion(HandleScope& scope,
                                const runtime::FutureResult& result,
                                runtime::Value& output) {
  if (result.state == runtime::FutureState::Pending) {
    return std::unexpected(
        Error{.status = ExecutionStatus::AsyncIdle, .message = {}, .stack_trace = {}});
  }
  if (result.state == runtime::FutureState::Failed) {
    return std::unexpected(CaptureHostError(scope.isolate(), result.diagnostic.status, result.value,
                                            result.diagnostic));
  }
  StoreHostResult(scope.isolate().heap(), output, result.value);
  return {};
}

Result<void> WaitHostFuture(HandleScope& scope,
                            runtime::Value future,
                            WaitMode mode,
                            runtime::Value& output) {
  auto& isolate = scope.isolate();
  auto& source = *runtime::Heap::GetFuture(*future.AsObject());
  source.MarkObserved();
  const auto pump = mode == WaitMode::WaitForHost ? runtime::FuturePumpMode::WaitForHost
                                                  : runtime::FuturePumpMode::AvailableWork;
  std::ignore = isolate.scheduler().PumpUntil(source, pump);
  if (isolate.native_control().IsStopRequested()) {
    return std::unexpected(
        Error{.status = ExecutionStatus::ExecutionStopped, .message = {}, .stack_trace = {}});
  }
  return ReadHostCompletion(scope, source.result(), output);
}

Result<void> GetHostFuture(HandleScope& scope, runtime::Value completer, runtime::Value& output) {
  auto& object = *completer.AsObject();
  const auto* signature = runtime::Heap::TypeOf(completer).signature;
  return PublishFuture(scope,
                       scope.isolate().heap().AllocateFuture(*runtime::Heap::GetCompleter(object)),
                       {
                           .kind = runtime::ValueKind::Object,
                           .object_kind = runtime::ObjectKind::Future,
                           .signature = signature,
                       },
                       output);
}

Result<void> CompleteHostFuture(runtime::HeapObject& completer, runtime::Value value) {
  const auto& type = runtime::Heap::TypeOf(runtime::Value::Object(&completer)).signature->result;
  if (type ? !runtime::Heap::IsValueOfType(value, *type) : !value.IsNull()) {
    return std::unexpected(
        Error{.status = ExecutionStatus::InvalidCast, .message = {}, .stack_trace = {}});
  }
  return CompletionResult(runtime::Heap::GetCompleter(completer)->Complete(value));
}

Result<void> AdoptHostFuture(runtime::HeapObject& completer, runtime::Value future) {
  auto type = runtime::Heap::TypeOf(runtime::Value::Object(&completer));
  type.object_kind = runtime::ObjectKind::Future;
  if (!runtime::Heap::IsValueOfType(future, type)) {
    return std::unexpected(
        Error{.status = ExecutionStatus::InvalidCast, .message = {}, .stack_trace = {}});
  }
  return CompletionResult(
      runtime::Heap::GetCompleter(completer)->Adopt(*runtime::Heap::GetFuture(*future.AsObject())));
}

Result<void> FailHostFuture(HandleScope& scope, runtime::Value completer, Error error) {
  const auto& root = HandleAccess::ReadErrorPayload(error);
  if (!root.IsEmpty() && &root.isolate() != &scope.isolate()) {
    return std::unexpected(
        Error{.status = ExecutionStatus::InvalidIsolate, .message = {}, .stack_trace = {}});
  }
  auto& future = *runtime::Heap::GetCompleter(*completer.AsObject());
  return CompletionResult(
      future.CompleteError(root.IsEmpty() ? runtime::Value::Null() : root.value(),
                           {
                               .message = std::move(error.message),
                               .stack_trace = std::move(error.stack_trace),
                               .status = error.status,
                               .origin = error.origin,
                           }));
}

}  // namespace vela::internal
