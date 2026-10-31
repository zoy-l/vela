#include "vm/execution.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "base/execution_limits.h"
#include "base/mock_operation.h"
#include "runtime/async/future.h"
#include "runtime/exceptions/exception.h"
#include "runtime/heap/heap.h"
#include "runtime/testing/test_bridge.h"
#include "runtime/value/value.h"
#include "vm/bytecode.h"
#include "vm/execution_status.h"
#include "vm/program.h"

namespace vela::vm::internal {
namespace {

struct MockTraceExpansion final {
  runtime::Value source;
  runtime::Value output;
};

StepResult ExpandMockTrace(const Program& program,
                           runtime::Heap& heap,
                           MockTraceExpansion expansion) {
  const auto source = expansion.source;
  const auto output = expansion.output;
  const auto positions = runtime::ReadTraceValue(source);
  if (!positions || !output.IsObject()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* layout = runtime::Heap::ArrayTypeOf(*output.AsObject());
  if (layout == nullptr || !layout->element.dynamic) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto frames = program.ResolveTrace(*positions);
  if (!frames) {
    return std::unexpected(frames.error());
  }
  auto& array = *output.AsObject();
  constexpr std::size_t kFrameWidth = 5;
  const auto length = *runtime::Heap::ArrayLength(array);
  if (!heap.ReserveArray(array, length + (frames->size() * kFrameWidth))) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  for (const auto& frame : *frames) {
    for (const auto& text : {frame.function, frame.source_path}) {
      auto* string = heap.AllocateString(text);
      if (string == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      std::ignore = heap.PushArray(array, runtime::Value::String(string));
    }
    for (const auto number : {
             static_cast<std::int64_t>(frame.location.line),
             static_cast<std::int64_t>(frame.location.column),
             static_cast<std::int64_t>(frame.instruction),
         }) {
      std::ignore = heap.PushArray(array, runtime::Value::Integer(number));
    }
  }
  return {};
}

}  // namespace

StepResult Execution::ExecuteMockInspection(ActiveFrame& active, Instruction instruction) {
  const auto registers = active.frame.registers();
  const auto arguments = registers.subspan(DecodeC(instruction), 3);
  const auto opcode = arguments.front().Read();
  const auto value = arguments.subspan(2).front().Read();
  auto destination = registers.subspan(DecodeA(instruction)).front();
  if (!opcode.IsInteger() || opcode.AsInteger() < 0 ||
      opcode.AsInteger() > static_cast<std::int64_t>(MockOperation::AcceptsResponseSignature)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto operation = static_cast<MockOperation>(opcode.AsInteger());
  const auto signature_value = arguments.subspan(1).front().Read();
  const MockInspection inspection{
      .operation = operation,
      .signature = signature_value,
      .value = value,
      .destination = destination,
  };
  if (operation == MockOperation::ObserveMatch || operation == MockOperation::FailObservation) {
    return ExecuteMockObservation(inspection);
  }
  if (operation <= MockOperation::AcceptsVoidAnswer ||
      operation == MockOperation::AcceptsResponseSignature) {
    return ExecuteMockCallable(inspection);
  }
  if (operation == MockOperation::CaptureTrace) {
    std::array<runtime::TracePosition, kMaxCallFrames> positions;
    const auto captured = runtime::CaptureTraceValue(isolate_.heap(), CapturePositions(positions));
    if (!captured) {
      return std::unexpected(captured.error());
    }
    destination = *captured;
    return {};
  }
  if (operation == MockOperation::ResolveTrace) {
    const auto expanded =
        ExpandMockTrace(program_, isolate_.heap(), {.source = signature_value, .output = value});
    if (!expanded) {
      return expanded;
    }
    destination.SetNull();
    return {};
  }
  if (operation == MockOperation::RegisterFailure) {
    if (!signature_value.IsInteger() || signature_value.AsInteger() < 0 ||
        signature_value.AsInteger() > static_cast<std::int64_t>(MockFailureKind::SessionClosed) ||
        !runtime::Heap::IsThrowable(value)) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    if (auto* bridge = isolate_.test_bridge()) {
      const auto& nominal = runtime::Heap::ClassOf(*value.AsObject())->nominal_type();
      bridge->RegisterMockFailure(nominal,
                                  static_cast<MockFailureKind>(signature_value.AsInteger()));
    }
    destination.SetNull();
    return {};
  }
  if (operation == MockOperation::IsThrowable) {
    destination.SetBoolean(runtime::Heap::IsThrowable(value));
    return {};
  }
  std::unreachable();
}

StepResult Execution::ExecuteMockObservation(MockInspection inspection) const {
  auto [operation, signature_value, value, destination] = inspection;
  if (operation == MockOperation::ObserveMatch) {
    auto* source = signature_value.IsObject()
                       ? runtime::Heap::GetFuture(*signature_value.AsObject())
                       : nullptr;
    auto* target = value.IsObject() ? runtime::Heap::GetCompleter(*value.AsObject()) : nullptr;
    if (source == nullptr || target == nullptr || source->is_pending()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto& completed = source->result();
    if (completed.state == runtime::FutureState::Failed) {
      source->ObserveResult([](const runtime::FutureResult&) {});
      auto diagnostic = completed.diagnostic;
      diagnostic.origin.context = target->creation_context();
      diagnostic.origin.captured = true;
      const auto forwarded = target->CompleteError(completed.value, std::move(diagnostic));
      if (!forwarded) {
        return std::unexpected(runtime::FutureCompletionStatus(forwarded.error()));
      }
      destination.SetBoolean(false);
    } else {
      if (!completed.value.IsBoolean()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      destination = completed.value;
    }
    return {};
  }
  auto* target = signature_value.IsObject()
                     ? runtime::Heap::GetCompleter(*signature_value.AsObject())
                     : nullptr;
  if (target == nullptr || !runtime::Heap::IsThrowable(value)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto diagnostic = ThrowDiagnostic(value);
  diagnostic.origin.context = target->creation_context();
  const auto failed = target->CompleteError(value, std::move(diagnostic));
  if (!failed) {
    return std::unexpected(runtime::FutureCompletionStatus(failed.error()));
  }
  destination.SetNull();
  return {};
}

bool Execution::IsMockResponseCompatible(const FunctionStorage& function,
                                         runtime::Value value) const {
  const auto& signature = *function.signature;
  const auto response = ResolveCallable(value);
  if (!response || !signature.result) {
    return false;
  }
  const auto& callback = functions_.subspan(response->function).front();
  const auto& callback_signature = callback.signature;
  if (!callback_signature || !callback_signature->result) {
    return false;
  }
  const auto& returned = callback.View().types.subspan(*callback_signature->result).front();
  return returned.dynamic ||
         returned.IsSubtypeOf(function.View().types.subspan(*signature.result).front());
}

StepResult Execution::ExecuteMockCallable(MockInspection inspection) {
  auto [operation, signature_value, value, destination] = inspection;
  const auto callable = ResolveCallable(signature_value);
  if (!callable) {
    destination.SetBoolean(false);
    return {};
  }
  const auto contract = ResolveCallableContract(*callable);
  if (!contract) {
    return std::unexpected(contract.error());
  }
  const auto parameters = contract->parameters;
  const auto& function = functions_.subspan(callable->function).front();
  const auto& signature = *contract->signature;
  const auto types = function.View().types;
  switch (operation) {
    case MockOperation::AcceptsResponseSignature:
      destination.SetBoolean(IsMockResponseCompatible(function, value));
      return {};
    case MockOperation::IsVoid:
      destination.SetBoolean(!signature.result);
      return {};
    case MockOperation::AcceptsReturn:
      destination.SetBoolean(
          signature.result &&
          runtime::Heap::IsValueOfType(value, types.subspan(*signature.result).front()));
      return {};
    case MockOperation::Arity:
      destination.SetInteger(static_cast<std::int64_t>(parameters.size()));
      return {};
    case MockOperation::ParameterName: {
      if (!value.IsInteger() || value.AsInteger() < 0 ||
          static_cast<std::uint64_t>(value.AsInteger()) >= parameters.size()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      auto* name = isolate_.heap().AllocateString(
          parameters.subspan(static_cast<std::size_t>(value.AsInteger())).front().name);
      if (name == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      destination.SetString(name);
      return {};
    }
    case MockOperation::AcceptsAnswer:
    case MockOperation::AcceptsVoidAnswer: {
      const bool valid_result = operation == MockOperation::AcceptsAnswer
                                    ? signature.result.has_value()
                                    : !signature.result && !function.is_async;
      const bool valid_parameters =
          !parameters.empty() &&
          runtime::Heap::IsValueOfType(value, types.subspan(parameters.front().type).front()) &&
          std::ranges::all_of(parameters.subspan(1), [](const auto& parameter) {
            return parameter.optional || parameter.rest;
          });
      destination.SetBoolean(valid_result && valid_parameters);
      return {};
    }
    default:
      return std::unexpected(ExecutionStatus::TypeError);
  }
}

}  // namespace vela::vm::internal
