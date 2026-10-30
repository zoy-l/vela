#include "vm/execution.h"

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>

#include "base/async_operation.h"
#include "runtime/async_state.h"
#include "runtime/callable.h"
#include "runtime/future_composition.h"
#include "runtime/future_operations.h"
#include "runtime/object.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/execution_status.h"
#include "vm/program.h"

namespace vela::vm::internal {

StepResult Execution::ExecuteAsync(ActiveFrame& active, Instruction instruction) {
  const auto operation = static_cast<AsyncOperation>(DecodeB(instruction));
  const auto registers = active.frame.registers();
  AsyncArguments call{
      .operation = operation,
      .values = registers.subspan(DecodeC(instruction), AsyncArity(operation)),
      .destination = registers.subspan(DecodeA(instruction)).front(),
  };
  const auto& layouts = *active.function->layouts;
  if (operation == AsyncOperation::Then || operation == AsyncOperation::CatchError ||
      operation == AsyncOperation::Timeout) {
    const auto index = call.At(AsyncArity(operation) - 1);
    const auto types = layouts.types();
    if (!index.IsInteger() || index.AsInteger() < 0 ||
        static_cast<std::size_t>(index.AsInteger()) >= types.size()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    call.runtime_type = types.subspan(static_cast<std::size_t>(index.AsInteger())).front();
  }
  if (IsAsyncStateOperation(operation)) {
    return ExecuteAsyncState(call);
  }
  const runtime::ArrayLayout* wait_layout = nullptr;
  if (operation == AsyncOperation::Wait) {
    const auto index = call.At(3);
    const auto arrays = layouts.arrays();
    if (!index.IsInteger() || index.AsInteger() < -1 ||
        (index.AsInteger() >= 0 && static_cast<std::size_t>(index.AsInteger()) >= arrays.size())) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    if (index.AsInteger() >= 0) {
      wait_layout = arrays.subspan(static_cast<std::size_t>(index.AsInteger())).front();
    }
  }
  return ExecuteFutureCall(call, wait_layout);
}

StepResult Execution::ExecuteAsyncState(const AsyncArguments& call) {
  std::array<runtime::Value, kMaximumAsyncArity> storage;
  const auto arguments = std::span(storage).first(call.values.size());
  call.values.CopyTo(arguments);
  const auto diagnostic = call.operation == AsyncOperation::CompleterError
                              ? ThrowDiagnostic(call.At(1))
                              : runtime::FutureDiagnostic();
  const auto result = runtime::ExecuteAsyncState(call.operation, arguments, isolate_, diagnostic);
  if (!result) {
    return std::unexpected(result.error());
  }
  auto destination = call.destination;
  destination = *result;
  return {};
}

StepResult Execution::ExecuteFutureCall(const AsyncArguments& call,
                                        const runtime::ArrayLayout* wait_layout) {
  std::array<runtime::Value, kMaximumAsyncArity> storage;
  const auto arguments = std::span(storage).first(call.values.size());
  call.values.CopyTo(arguments);
  auto description = DescribeFutureCall(call.operation, arguments);
  description.runtime_type = call.runtime_type;
  description.wait_layout = wait_layout;
  const auto result = runtime::ExecuteFutureOperation(isolate_, description, MakeFutureCallbacks());
  if (!result) {
    return std::unexpected(result.error());
  }
  auto destination = call.destination;
  destination = *result;
  return {};
}

runtime::FutureCallbacks Execution::MakeFutureCallbacks() {
  const auto program = program_.Internal();
  const auto debug_session = debug_session_;
  auto* isolate = &isolate_;
  return {
      .arity = [program](runtime::Value callback) -> std::expected<std::size_t, ExecutionStatus> {
        const auto functions = program.functions();
        const auto resolved =
            runtime::ResolveCallable(callback, program.code_identity(), functions.size());
        if (!resolved) {
          return std::unexpected(resolved.error());
        }
        return functions.subspan(resolved->function).front().parameter_count -
               (resolved->implicit ? 1U : 0U);
      },
      .invoke =
          [isolate, program, debug_session](runtime::Value callback,
                                            std::span<const runtime::Value> values) {
            return InvokeFuture(*isolate, program, callback, values, debug_session);
          },
  };
}

runtime::FutureCall Execution::DescribeFutureCall(AsyncOperation operation,
                                                  std::span<const runtime::Value> values) const {
  return {
      .operation = operation,
      .values = values,
      .retained =
          globals_ != nullptr ? std::span(&globals_->storage, 1) : std::span<runtime::Value>(),
      .type_error = Diagnostic({
          .status = ExecutionStatus::TypeError,
          .stack_trace = CaptureStack(),
          .message = "Future callback result does not match its result type",
          .origin = {},
      }),
      .timeout_error = Diagnostic({
          .status = ExecutionStatus::Thrown,
          .stack_trace = CaptureStack(),
          .message = "Future timed out",
          .origin = {},
      }),
  };
}

std::expected<runtime::Value, ExecutionStatus> Execution::ValidateNativeFuture(
    runtime::Value value,
    std::optional<runtime::TypeConstraint> payload_type) {
  const auto diagnostic = Diagnostic({
      .status = ExecutionStatus::TypeError,
      .stack_trace = CaptureStack(),
      .message = "native Future completion does not match its external declaration",
      .origin = {},
  });
  const auto retained =
      globals_ != nullptr ? std::span(&globals_->storage, 1) : std::span<runtime::Value>();
  return runtime::CheckFutureCompletion(isolate_, value, payload_type, diagnostic, retained);
}

}  // namespace vela::vm::internal
