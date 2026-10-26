#include "base/builtin.h"
#include "vm/execution.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include "base/async_operation.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/execution_status.h"

namespace vela::vm::internal {
namespace {

using runtime::Heap;
using runtime::TypeConstraint;
using runtime::TypeSignature;
using runtime::Value;

bool IsThrowableType(const TypeConstraint& type) {
  return !type.nullable && !type.dynamic && type.kind == runtime::ValueKind::Object &&
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

StepResult CheckFutureCallbacks(AsyncArguments& call, const TypeSignature& future) {
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
               ? StepResult{}
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
    call.runtime_type = TypeConstraint{.kind = runtime::ValueKind::Object};
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

StepResult CheckFutureCall(AsyncArguments& call,
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

StepResult CheckCompleterCall(const AsyncArguments& call, const TypeConstraint& type) {
  if (type.signature == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto operation = call.operation;
  if (operation == AsyncOperation::CompleterComplete) {
    auto future = type;
    future.object_kind = runtime::ObjectKind::Future;
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

StepResult Execution::ExecuteDynamicAsync(ActiveFrame& active, Instruction instruction) {
  const auto values =
      active.frame.registers().subspan(DecodeC(instruction), kDynamicOperationArity);
  const auto selector = values.subspan(3).front().Read();
  if (!selector.IsInteger() || selector.AsInteger() < 0 ||
      selector.AsInteger() >= static_cast<std::int64_t>(AsyncOperation::Count)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  AsyncArguments call{
      .operation = static_cast<AsyncOperation>(selector.AsInteger()),
      .values = values.subspan(0, 4),
      .destination = active.frame.registers().subspan(DecodeA(instruction)).front(),
  };
  const auto receiver = call.At(0);
  const auto type = Heap::TypeOf(receiver);
  const auto operation = call.operation;
  if (type.object_kind == runtime::ObjectKind::Future) {
    if (auto checked = CheckFutureCall(call, type, active.function->layouts->types(),
                                       values.subspan(4).front().Read());
        !checked) {
      return checked;
    }
    return ExecuteAsyncComposition(call);
  }
  if (type.object_kind == runtime::ObjectKind::Completer) {
    if (auto checked = CheckCompleterCall(call, type); !checked) {
      return checked;
    }
    const auto executed = ExecuteCompleter(call);
    if (!executed || operation != AsyncOperation::CompleterFuture) {
      return executed;
    }
    auto future = type;
    future.object_kind = runtime::ObjectKind::Future;
    const auto* layout = isolate_.CreateArrayLayout({.element = future});
    return Heap::BindRuntimeType(*call.destination.AsObject(), *layout);
  }
  if ((type.object_kind == runtime::ObjectKind::Timer &&
       operation == AsyncOperation::TimerCancel) ||
      (type.object_kind == runtime::ObjectKind::CancellationToken &&
       (operation == AsyncOperation::CancellationCancel ||
        operation == AsyncOperation::CancellationIsCancelled))) {
    return ExecuteAsyncState(call);
  }
  return std::unexpected(ExecutionStatus::TypeError);
}

}  // namespace vela::vm::internal
