#include "runtime/dynamic_async.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include "base/async_operation.h"
#include "base/execution_status.h"
#include "runtime/async_state.h"
#include "runtime/future.h"
#include "runtime/future_composition.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"
#include "runtime/value_operations.h"

namespace vela::runtime {
namespace {

using OperationResult = std::expected<void, ExecutionStatus>;

bool IsThrowableType(const TypeConstraint& type) {
  return !type.nullable && !type.dynamic && type.kind == ValueKind::Object &&
         (type.interface != nullptr ||
          (type.instance != nullptr && !type.instance->IsEnum() && !type.instance->callable()));
}

std::expected<const TypeSignature*, ExecutionStatus> Callback(Value value, std::size_t count) {
  const auto type = Heap::TypeOf(value);
  if (!type.callable || type.signature == nullptr || type.signature->parameters.size() != count) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  for (const auto& parameter : type.signature->parameters) {
    if (parameter.optional || parameter.rest) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  return type.signature;
}

bool HasCompatibleResult(const TypeSignature& callback, std::optional<TypeConstraint> expected) {
  const auto result = callback.FlattenedResult();
  return result && expected ? (*result).dynamic || (*result).IsSubtypeOf(*expected)
                            : !result && !expected;
}

OperationResult CheckFutureCallbacks(FutureCall& call, const TypeSignature& future) {
  const auto operation = call.operation;
  if (operation == AsyncOperation::WhenComplete) {
    const auto callback = Callback(call.At(1), 0);
    if (!callback || (*callback)->FlattenedResult()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    return {};
  }
  if (operation == AsyncOperation::Timeout) {
    if (call.At(2).IsNull()) {
      return {};
    }
    const auto callback = Callback(call.At(2), 0);
    return callback && HasCompatibleResult(**callback, future.result)
               ? OperationResult{}
               : std::unexpected(ExecutionStatus::TypeError);
  }
  if (operation == AsyncOperation::Then) {
    const auto callback = Callback(call.At(1), future.result ? 1U : 0U);
    if (!callback || (future.result && !(*future.result).dynamic &&
                      !(*future.result).IsSubtypeOf((*callback)->parameters.front().type))) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  const auto error = call.At(operation == AsyncOperation::Then ? 2 : 1);
  if (error.IsNull() && operation == AsyncOperation::Then) {
    call.runtime_type = TypeConstraint{.kind = ValueKind::Object};
    return {};
  }
  const auto callback = Callback(error, 1);
  if (!callback) {
    return std::unexpected(callback.error());
  }
  const auto error_type = (*callback)->parameters.front().type;
  const auto* value_callback =
      operation == AsyncOperation::Then ? Heap::TypeOf(call.At(1)).signature : &future;
  const auto output =
      operation == AsyncOperation::Then ? value_callback->FlattenedResult() : future.result;
  if (!IsThrowableType(error_type) || !HasCompatibleResult(**callback, output)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  call.runtime_type = error_type;
  return {};
}

OperationResult CheckFutureCall(FutureCall& call,
                                const TypeConstraint& type,
                                std::span<const TypeConstraint> types,
                                Value timeout_index) {
  const auto operation = call.operation;
  if (type.signature == nullptr ||
      (operation != AsyncOperation::Then && operation != AsyncOperation::CatchError &&
       operation != AsyncOperation::WhenComplete && operation != AsyncOperation::Timeout)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (operation == AsyncOperation::Timeout) {
    if (!timeout_index.IsInteger() || timeout_index.AsInteger() < 0 ||
        static_cast<std::size_t>(timeout_index.AsInteger()) >= types.size()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    call.runtime_type = types.subspan(static_cast<std::size_t>(timeout_index.AsInteger())).front();
  }
  return CheckFutureCallbacks(call, *type.signature);
}

OperationResult CheckCompleterCall(const FutureCall& call, const TypeConstraint& type) {
  if (type.signature == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto operation = call.operation;
  if (operation == AsyncOperation::CompleterComplete) {
    auto future = type;
    future.object_kind = ObjectKind::Future;
    const auto payload = call.At(1);
    const auto expected = type.signature->result;
    if (!Heap::IsValueOfType(payload, future) &&
        !(expected ? Heap::IsValueOfType(payload, *expected) : payload.IsNull())) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  } else if (operation == AsyncOperation::CompleterError) {
    if (!Heap::IsThrowable(call.At(1))) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  } else if (operation != AsyncOperation::CompleterFuture &&
             operation != AsyncOperation::CompleterIsCompleted) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return {};
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteDynamicAsyncOperation(
    Isolate& isolate,
    FutureCall call,
    std::span<const TypeConstraint> types,
    const FutureCallbacks& callbacks,
    const FutureDiagnostic& completion_error) {
  const auto input = call.values;
  const auto selector = input.subspan(3).front();
  if (!selector.IsInteger() || selector.AsInteger() < 0 ||
      selector.AsInteger() >= static_cast<std::int64_t>(AsyncOperation::Count)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto operation = static_cast<AsyncOperation>(selector.AsInteger());
  call.operation = operation;
  call.values = input.first(4);
  const auto type = Heap::TypeOf(call.At(0));
  if (type.object_kind == ObjectKind::Future) {
    const auto checked = CheckFutureCall(call, type, types, input.subspan(4).front());
    if (!checked) {
      return std::unexpected(checked.error());
    }
    return ExecuteFutureOperation(isolate, call, callbacks);
  }
  if (type.object_kind == ObjectKind::Completer) {
    const auto checked = CheckCompleterCall(call, type);
    if (!checked) {
      return std::unexpected(checked.error());
    }
    const auto value = ExecuteAsyncState(operation, call.values, isolate, completion_error);
    if (!value || operation != AsyncOperation::CompleterFuture) {
      return value;
    }
    auto future = type;
    future.object_kind = ObjectKind::Future;
    return BindValueType(*value, future);
  }
  if ((type.object_kind == ObjectKind::Timer && operation == AsyncOperation::TimerCancel) ||
      (type.object_kind == ObjectKind::CancellationToken &&
       (operation == AsyncOperation::CancellationCancel ||
        operation == AsyncOperation::CancellationIsCancelled))) {
    return ExecuteAsyncState(operation, call.values, isolate, completion_error);
  }
  return std::unexpected(ExecutionStatus::TypeError);
}

}  // namespace vela::runtime
