#include "runtime/async/async_state.h"

#include <expected>
#include <span>

#include "base/async_operation.h"
#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

std::expected<Value, ExecutionStatus> Object(HeapObject* object) {
  return object != nullptr ? std::expected<Value, ExecutionStatus>(Value::Object(object))
                           : std::unexpected(ExecutionStatus::OutOfMemory);
}

std::expected<Value, ExecutionStatus> ExecuteCompleter(AsyncOperation operation,
                                                       std::span<const Value> arguments,
                                                       Isolate& isolate,
                                                       const FutureDiagnostic& error) {
  const auto first = arguments.front();
  auto* completer = first.IsObject() ? Heap::GetCompleter(*first.AsObject()) : nullptr;
  if (completer == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (operation == AsyncOperation::CompleterFuture) {
    return Object(isolate.heap().AllocateFuture(*completer));
  }
  if (operation == AsyncOperation::CompleterIsCompleted) {
    return Value::Boolean(completer->IsCompleted());
  }
  const auto value = arguments.subspan(1).front();
  if (operation == AsyncOperation::CompleterError && !Heap::IsThrowable(value)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* adopted = value.IsObject() ? Heap::GetFuture(*value.AsObject()) : nullptr;
  const auto completion = [&] {
    if (operation == AsyncOperation::CompleterError) {
      return completer->CompleteError(value, error);
    }
    return adopted != nullptr ? completer->Adopt(*adopted) : completer->Complete(value);
  }();
  if (!completion) {
    return std::unexpected(FutureCompletionStatus(completion.error()));
  }
  return Value::Null();
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteAsyncState(AsyncOperation operation,
                                                        std::span<const Value> arguments,
                                                        Isolate& isolate,
                                                        const FutureDiagnostic& error) {
  auto& heap = isolate.heap();
  if (operation == AsyncOperation::NewCompleter) {
    return Object(heap.AllocateCompleter(Future(&isolate.scheduler())));
  }
  if (operation == AsyncOperation::NewCancellationToken) {
    return Object(heap.AllocateCancellationToken(CancellationToken()));
  }
  const auto first = arguments.front();
  if (operation == AsyncOperation::CancellationCancel ||
      operation == AsyncOperation::CancellationIsCancelled) {
    auto* token = first.IsObject() ? Heap::GetCancellationToken(*first.AsObject()) : nullptr;
    if (token == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    if (operation == AsyncOperation::CancellationCancel) {
      token->Cancel();
      return Value::Null();
    }
    return Value::Boolean(token->IsCancelled());
  }
  if (operation == AsyncOperation::TimerCancel) {
    auto* timer = first.IsObject() ? Heap::GetTimer(*first.AsObject()) : nullptr;
    if (timer == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    timer->Cancel();
    return Value::Null();
  }
  return ExecuteCompleter(operation, arguments, isolate, error);
}

}  // namespace vela::runtime
