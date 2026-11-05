#include "runtime/native/native_mock.h"
#include <optional>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include "base/execution_status.h"
#include "base/mock_operation.h"
#include "runtime/async/future.h"
#include "runtime/async/future_operations.h"
#include "runtime/exceptions/error.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/testing/test_bridge.h"
#include "runtime/testing/trace_value.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {
namespace {

std::expected<Value, ExecutionStatus> ExpandTrace(NativeExecution& execution,
                                                  std::span<const FutureTraceFrame> frames,
                                                  Value callback) {
  auto& heap = execution.isolate().heap();
  RootRange callback_root(heap, &callback, 1, std::nullopt);
  constexpr std::size_t kFrameWidth = 5;
  std::array<Value, kFrameWidth> arguments;
  RootRange argument_roots(heap, arguments.data(), arguments.size(), std::nullopt);
  for (const auto& frame : frames) {
    const std::array texts{std::string_view(frame.function), std::string_view(frame.source_path)};
    for (std::size_t index = 0; index < texts.size(); ++index) {
      const auto text = texts[index];
      auto* string = heap.AllocateString(text);
      if (string == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      arguments[index] = Value::String(string);
    }
    arguments[2] = Value::Integer(static_cast<std::int64_t>(frame.line));
    arguments[3] = Value::Integer(static_cast<std::int64_t>(frame.column));
    arguments[4] = Value::Integer(static_cast<std::int64_t>(frame.instruction));
    if (const auto invoked = InvokeNativeCallable(execution, callback, arguments); !invoked) {
      return invoked;
    }
  }
  return Value::Null();
}

std::expected<Value, ExecutionStatus> Observe(NativeExecution& execution,
                                              MockOperation operation,
                                              Value source_value,
                                              Value value) {
  if (operation == MockOperation::ObserveMatch) {
    auto* source = AsFuture(source_value);
    auto* target = value.IsObject() ? Heap::GetCompleter(*value.AsObject()) : nullptr;
    if (source == nullptr || target == nullptr || source->is_pending()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto& completed = source->result();
    if (completed.state == FutureState::Failed) {
      source->MarkObserved();
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
  if (!response || !result) {
    return false;
  }
  const auto response_result = response->signature->result;
  if (!response_result) {
    return false;
  }
  const auto& returned = response->types.subspan(*response_result).front();
  return returned.dynamic || returned.IsSubtypeOf(contract.types.subspan(*result).front());
}

std::expected<Value, ExecutionStatus> Inspect(NativeExecution& execution,
                                              MockOperation operation,
                                              const NativeCallableContract& contract,
                                              Value value) {
  const auto& instance = execution.instance();
  const auto parameters = contract.parameters;
  const auto& signature = *contract.signature;
  const auto types = contract.types;
  switch (operation) {
    case MockOperation::AcceptsResponseSignature:
      return Value::Boolean(IsResponseCompatible(instance, contract, value));
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
      auto* name =
          execution.isolate().heap().AllocateString(parameters.subspan(index).front().name);
      return name != nullptr ? std::expected<Value, ExecutionStatus>(Value::String(name))
                             : std::unexpected(ExecutionStatus::OutOfMemory);
    }
    case MockOperation::AcceptsAnswer:
    case MockOperation::AcceptsVoidAnswer: {
      const auto valid_result = operation == MockOperation::AcceptsAnswer
                                    ? signature.result.has_value()
                                    : !signature.result && !contract.is_async;
      const auto valid_parameters =
          !parameters.empty() &&
          Heap::IsValueOfType(value, types.subspan(parameters.front().type).front()) &&
          !contract.HasRequiredParameters(1);
      return Value::Boolean(valid_result && valid_parameters);
    }
    default:
      return std::unexpected(ExecutionStatus::TypeError);
  }
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteNativeMock(NativeExecution& execution,
                                                        std::span<const Value> arguments) {
  const auto decoded = DecodeOperation<MockOperation::AcceptsResponseSignature>(arguments.front());
  if (!decoded) {
    return std::unexpected(decoded.error());
  }
  const auto operation = *decoded;
  const auto signature_value = arguments.subspan(1).front();
  const auto value = arguments.subspan(2).front();
  if (operation == MockOperation::ObserveMatch || operation == MockOperation::FailObservation) {
    return Observe(execution, operation, signature_value, value);
  }
  if (operation <= MockOperation::AcceptsVoidAnswer ||
      operation == MockOperation::AcceptsResponseSignature) {
    const auto contract = InspectNativeCallable(execution.instance(), signature_value);
    if (contract) {
      return Inspect(execution, operation, *contract, value);
    }
    if (operation == MockOperation::Arity) {
      return Value::Null();
    }
    if (operation == MockOperation::ParameterName) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    return Value::Boolean(false);
  }
  auto& isolate = execution.isolate();
  if (operation == MockOperation::CaptureTrace) {
    return CaptureTestTrace(isolate.heap(), execution.CaptureTrace());
  }
  if (operation == MockOperation::ResolveTrace) {
    const auto frames = ReadTestTrace(signature_value);
    if (!frames) {
      return std::unexpected(frames.error());
    }
    return ExpandTrace(execution, *frames, value);
  }
  if (operation == MockOperation::RegisterFailure) {
    const auto kind = DecodeOperation<MockFailureKind::SessionClosed>(signature_value);
    if (!kind || !Heap::IsThrowable(value)) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    if (auto* bridge = isolate.test_bridge()) {
      bridge->RegisterMockFailure(Heap::ClassOf(*value.AsObject())->nominal_type(), *kind);
    }
    return Value::Null();
  }
  if (operation == MockOperation::IsThrowable) {
    return Value::Boolean(Heap::IsThrowable(signature_value));
  }
  std::unreachable();
}

}  // namespace vela::runtime
