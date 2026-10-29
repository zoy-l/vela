#include "runtime/future_operations.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "base/execution_status.h"
#include "runtime/error.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

std::expected<Value, ExecutionStatus> AllocateFutureValue(Isolate& isolate, Value value) {
  auto* source = value.IsObject() ? Heap::GetFuture(*value.AsObject()) : nullptr;
  auto future = source != nullptr ? source->Rooted() : Future::Value(value, &isolate.scheduler());
  auto* object = isolate.heap().AllocateFuture(std::move(future));
  return object != nullptr ? std::expected<Value, ExecutionStatus>(Value::Object(object))
                           : std::unexpected(ExecutionStatus::OutOfMemory);
}

std::expected<Value, ExecutionStatus> AllocateFutureError(Isolate& isolate,
                                                          Value value,
                                                          const FutureDiagnostic& diagnostic) {
  if (!Heap::IsThrowable(value)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  Future future(&isolate.scheduler());
  std::ignore = future.CompleteError(value, diagnostic);
  auto* object = isolate.heap().AllocateFuture(std::move(future));
  return object != nullptr ? std::expected<Value, ExecutionStatus>(Value::Object(object))
                           : std::unexpected(ExecutionStatus::OutOfMemory);
}

std::expected<Value, ExecutionStatus> AllocateSleepFuture(Isolate& isolate,
                                                          std::int64_t milliseconds) {
  const auto delay = std::chrono::milliseconds(milliseconds);
  if (!FutureScheduler::CanSchedule(delay)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto* object = isolate.heap().AllocateFuture(Future::Sleep(delay, isolate.scheduler()));
  return object != nullptr ? std::expected<Value, ExecutionStatus>(Value::Object(object))
                           : std::unexpected(ExecutionStatus::OutOfMemory);
}

void CompleteFuture(Isolate& isolate, Future& future, const Future& source) {
  const auto adopted = future.Adopt(source);
  if (adopted) {
    return;
  }
  const auto status = FutureCompletionStatus(adopted.error());
  const auto message = ExecutionStatusName(status);
  const auto payload = CreateError(isolate.heap(), isolate.error_layout(),
                                   {.message = message, .code = ExecutionStatusCode(status)});
  std::ignore =
      future.CompleteError(payload.value_or(Value::Null()),
                           {.message = std::string(message), .stack_trace = {}, .status = status});
}

void CompleteFutureError(Isolate& isolate,
                         Future& future,
                         Value payload,
                         const FutureDiagnostic& diagnostic) {
  const auto status = diagnostic.status;
  if (payload.IsNull() && status != ExecutionStatus::DebuggerStopped) {
    payload = CreateError(
                  isolate.heap(), isolate.error_layout(),
                  {
                      .message = diagnostic.message.empty() ? ExecutionStatusName(status)
                                                            : std::string_view(diagnostic.message),
                      .code = ExecutionStatusCode(status),
                  })
                  .value_or(Value::Null());
  }
  std::ignore = future.CompleteError(payload, diagnostic);
}

void ObserveCheckedCompletion(Isolate& isolate,
                              Future target,
                              Future& source,
                              std::optional<TypeConstraint> type,
                              const FutureDiagnostic& diagnostic,
                              const std::shared_ptr<std::vector<Value>>& references) {
  source.ObserveResult(
      [&isolate, target, type, diagnostic](const FutureResult& input) mutable {
        const auto value = input.value;
        const auto succeeded = input.state == FutureState::Succeeded;
        bool matches = Heap::IsThrowable(value);
        if (succeeded) {
          matches = type ? Heap::IsValueOfType(value, *type) : value.IsNull();
        }
        if (matches) {
          std::ignore =
              succeeded ? target.Complete(value) : target.CompleteError(value, input.diagnostic);
          return;
        }
        const auto error = CreateError(isolate.heap(), isolate.error_layout(),
                                       {.message = diagnostic.message, .code = "TypeError"});
        std::ignore = target.CompleteError(error.value_or(Value::Null()), diagnostic);
      },
      references);
}

std::expected<Value, ExecutionStatus> CheckFutureCompletion(Isolate& isolate,
                                                            Value source,
                                                            std::optional<TypeConstraint> payload,
                                                            const FutureDiagnostic& diagnostic,
                                                            std::span<const Value> retained) {
  RootRange source_root(isolate.heap(), &source, 1, RootOwner::Runtime);
  Future result(&isolate.scheduler());
  auto* object = isolate.heap().AllocateFuture(result);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  auto references = std::make_shared<std::vector<Value>>();
  references->reserve(retained.size() + 2);
  references->push_back(source);
  references->push_back(Value::Object(object));
  references->insert(references->end(), retained.begin(), retained.end());
  result.KeepAlive(references);
  ObserveCheckedCompletion(isolate, result.Internal(), *Heap::GetFuture(*source.AsObject()),
                           payload, diagnostic, references);
  return Value::Object(object);
}

}  // namespace vela::runtime
