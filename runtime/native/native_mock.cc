#include "runtime/native/native_mock.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "base/mock_operation.h"
#include "runtime/async/future.h"
#include "runtime/exceptions/error.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/testing/test_bridge.h"
#include "runtime/testing/trace_value.h"
#include "runtime/value/object.h"

namespace vela::runtime {
namespace {

std::expected<Value, ExecutionStatus> ExpandTrace(Heap& heap, Value source, Value output) {
  const auto frames = ReadTestTrace(source);
  if (!frames || !output.IsObject()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& array = *output.AsObject();
  const auto* layout = Heap::ArrayTypeOf(array);
  if (layout == nullptr || !layout->element.dynamic) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  constexpr std::size_t kFrameWidth = 5;
  if (!heap.ReserveArray(array, *Heap::ArrayLength(array) + frames->size() * kFrameWidth)) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  for (const auto& frame : *frames) {
    for (const auto& text : {frame.function, frame.source_path}) {
      auto* string = heap.AllocateString(text);
      if (string == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      std::ignore = heap.PushArray(array, Value::String(string));
    }
    for (const auto number : {static_cast<std::int64_t>(frame.line),
                              static_cast<std::int64_t>(frame.column),
                              static_cast<std::int64_t>(frame.instruction)}) {
      std::ignore = heap.PushArray(array, Value::Integer(number));
    }
  }
  return Value::Null();
}

std::expected<Value, ExecutionStatus> Observe(NativeExecution& execution,
                                              MockOperation operation,
                                              Value source_value,
                                              Value value) {
  if (operation == MockOperation::ObserveMatch) {
    auto* source = source_value.IsObject() ? Heap::GetFuture(*source_value.AsObject()) : nullptr;
    auto* target = value.IsObject() ? Heap::GetCompleter(*value.AsObject()) : nullptr;
    if (source == nullptr || target == nullptr || source->is_pending()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto& completed = source->result();
    if (completed.state == FutureState::Failed) {
      source->ObserveResult([](const FutureResult&) {});
      auto diagnostic = completed.diagnostic;
      diagnostic.origin.context = target->creation_context();
      diagnostic.origin.captured = true;
      const auto forwarded = target->CompleteError(completed.value, std::move(diagnostic));
      if (!forwarded) {
        return std::unexpected(FutureCompletionStatus(forwarded.error()));
      }
      return Value::Boolean(false);
    }
    if (!completed.value.IsBoolean()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    return completed.value;
  }
  auto* target = source_value.IsObject() ? Heap::GetCompleter(*source_value.AsObject()) : nullptr;
  if (target == nullptr || !Heap::IsThrowable(value)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto diagnostic = execution.Diagnostic(ExecutionStatus::Thrown,
                                         ErrorMessage(value, execution.isolate().error_layout()));
  diagnostic.origin.context = target->creation_context();
  const auto failed = target->CompleteError(value, std::move(diagnostic));
  if (!failed) {
    return std::unexpected(FutureCompletionStatus(failed.error()));
  }
  return Value::Null();
}

bool IsResponseCompatible(const NativeInstance& instance,
                          const NativeCallableContract& contract,
                          Value value) {
  const auto response = InspectNativeCallable(instance, value);
  const auto result = contract.signature->result;
  if (!response || !result || !response->signature->result) {
    return false;
  }
  const auto& returned = response->types.subspan(*response->signature->result).front();
  return returned.dynamic || returned.IsSubtypeOf(contract.types.subspan(*result).front());
}

std::expected<Value, ExecutionStatus> Inspect(NativeExecution& execution,
                                              MockOperation operation,
                                              Value signature_value,
                                              Value value) {
  const auto& instance = execution.instance();
  const auto contract = InspectNativeCallable(instance, signature_value);
  if (!contract) {
    return Value::Boolean(false);
  }
  const auto parameters = contract->parameters;
  const auto& signature = *contract->signature;
  const auto types = contract->types;
  switch (operation) {
    case MockOperation::AcceptsResponseSignature:
      return Value::Boolean(IsResponseCompatible(instance, *contract, value));
    case MockOperation::IsVoid:
      return Value::Boolean(!signature.result);
    case MockOperation::AcceptsReturn:
      return Value::Boolean(signature.result &&
                            Heap::IsValueOfType(value, types.subspan(*signature.result).front()));
    case MockOperation::Arity:
      return Value::Integer(static_cast<std::int64_t>(parameters.size()));
    case MockOperation::ParameterName: {
      if (!value.IsInteger() || value.AsInteger() < 0 ||
          std::cmp_greater_equal(value.AsInteger(), parameters.size())) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      const auto index = static_cast<std::size_t>(value.AsInteger());
      auto* name = execution.isolate().heap().AllocateString(parameters.subspan(index).front().name);
      return name != nullptr ? std::expected<Value, ExecutionStatus>(Value::String(name))
                             : std::unexpected(ExecutionStatus::OutOfMemory);
    }
    case MockOperation::AcceptsAnswer:
    case MockOperation::AcceptsVoidAnswer: {
      const auto valid_result = operation == MockOperation::AcceptsAnswer
                                    ? signature.result.has_value()
                                    : !signature.result && !contract->is_async;
      const auto valid_parameters =
          !parameters.empty() &&
          Heap::IsValueOfType(value, types.subspan(parameters.front().type).front()) &&
          std::ranges::all_of(parameters.subspan(1), [](const auto& parameter) {
            return parameter.optional || parameter.rest;
          });
      return Value::Boolean(valid_result && valid_parameters);
    }
    default:
      return std::unexpected(ExecutionStatus::TypeError);
  }
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteNativeMock(NativeExecution& execution,
                                                        std::span<const Value> arguments) {
  const auto opcode = arguments.front();
  if (!opcode.IsInteger() || opcode.AsInteger() < 0 ||
      opcode.AsInteger() > static_cast<std::int64_t>(MockOperation::AcceptsResponseSignature)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto operation = static_cast<MockOperation>(opcode.AsInteger());
  const auto signature_value = arguments.subspan(1).front();
  const auto value = arguments.subspan(2).front();
  if (operation == MockOperation::ObserveMatch || operation == MockOperation::FailObservation) {
    return Observe(execution, operation, signature_value, value);
  }
  if (operation <= MockOperation::AcceptsVoidAnswer ||
      operation == MockOperation::AcceptsResponseSignature) {
    return Inspect(execution, operation, signature_value, value);
  }
  auto& isolate = execution.isolate();
  if (operation == MockOperation::CaptureTrace) {
    return CaptureTestTrace(isolate.heap(),
                            execution.Diagnostic(ExecutionStatus::Returned, {}).stack_trace);
  }
  if (operation == MockOperation::ResolveTrace) {
    return ExpandTrace(isolate.heap(), signature_value, value);
  }
  if (operation == MockOperation::RegisterFailure) {
    if (!signature_value.IsInteger() || signature_value.AsInteger() < 0 ||
        signature_value.AsInteger() > static_cast<std::int64_t>(MockFailureKind::SessionClosed) ||
        !Heap::IsThrowable(value)) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    if (auto* bridge = isolate.test_bridge()) {
      bridge->RegisterMockFailure(Heap::ClassOf(*value.AsObject())->nominal_type(),
                                  static_cast<MockFailureKind>(signature_value.AsInteger()));
    }
    return Value::Null();
  }
  if (operation == MockOperation::IsThrowable) {
    return Value::Boolean(Heap::IsThrowable(value));
  }
  std::unreachable();
}

}  // namespace vela::runtime
