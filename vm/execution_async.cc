#include "vm/execution.h"

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <utility>

#include "base/async_operation.h"
#include "runtime/async/async_state.h"
#include "runtime/async/future_composition.h"
#include "runtime/async/future_operations.h"
#include "runtime/value/callable.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "vm/bytecode.h"
#include "vm/execution_status.h"
#include "vm/program.h"

namespace vela::vm::internal {

StepResult Execution::ExecuteAsync(ActiveFrame& active, Instruction instruction) {
  const auto operation = static_cast<AsyncOperation>(DecodeB(instruction));
  const auto registers = active.frame.registers();
  const auto count = AsyncArity(operation);
  std::array<runtime::Value, kMaximumAsyncArity> storage;
  const auto arguments = std::span(storage).first(count);
  registers.subspan(DecodeC(instruction), count).CopyTo(arguments);
  const auto destination = registers.subspan(DecodeA(instruction)).front();
  if (IsAsyncStateOperation(operation)) {
    const auto diagnostic = operation == AsyncOperation::CompleterError
                                ? ThrowDiagnostic(arguments.subspan(1).front())
                                : runtime::FutureDiagnostic();
    return StoreResult(destination,
                       runtime::ExecuteAsyncState(operation, arguments, isolate_, diagnostic));
  }
  const auto& layouts = *active.function->layouts;
  std::optional<runtime::TypeConstraint> runtime_type;
  if (operation == AsyncOperation::Then || operation == AsyncOperation::CatchError ||
      operation == AsyncOperation::Timeout) {
    const auto index = arguments.back();
    const auto types = layouts.types();
    const auto offset = ReadLayoutIndex(index, types.size());
    if (!offset) {
      return std::unexpected(offset.error());
    }
    runtime_type = types.subspan(*offset).front();
  }
  const runtime::ArrayLayout* wait_layout = nullptr;
  if (operation == AsyncOperation::Wait) {
    const auto index = arguments.subspan(3).front();
    const auto arrays = layouts.arrays();
    if (!index.IsInteger()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto offset = index.AsInteger();
    if (offset < -1 || (offset >= 0 && std::cmp_greater_equal(offset, arrays.size()))) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    if (offset >= 0) {
      wait_layout = arrays.subspan(static_cast<std::size_t>(offset)).front();
    }
  }
  auto call = DescribeFutureCall(operation, arguments);
  call.runtime_type = runtime_type;
  call.wait_layout = wait_layout;
  return StoreResult(destination,
                     runtime::ExecuteFutureOperation(isolate_, call, MakeFutureCallbacks()));
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
