#include "base/execution_status.h"
#include "vm/execution.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/execution_limits.h"
#include "base/source_location.h"
#include "runtime/error.h"
#include "runtime/exception.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/debug_info.h"
#include "vm/exception_handler.h"
#include "vm/execution_status.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::vm::internal {
namespace {

const ExceptionHandler* FindHandler(std::span<const ExceptionHandler> handlers,
                                    std::size_t instruction) {
  const auto next = std::ranges::upper_bound(handlers, instruction, {}, &ExceptionHandler::begin);
  if (next == handlers.begin()) {
    return nullptr;
  }
  const auto& handler = *std::prev(next);
  return instruction < handler.end ? &handler : nullptr;
}

bool IsException(runtime::Value value) {
  return runtime::Heap::IsValueOfType(
      value, {.kind = runtime::ValueKind::Object, .object_kind = runtime::ObjectKind::Exception});
}

}  // namespace

std::span<const runtime::TracePosition> Execution::CapturePositions(
    std::span<runtime::TracePosition> storage) const {
  std::size_t count = 0;
  for (auto depth = frame_count_; depth != 0; --depth) {
    const auto& active = frames_.at(depth - 1);
    storage.subspan(count++).front() = {
        .function = static_cast<std::uint32_t>(active.function - functions_.data()),
        .instruction = static_cast<std::uint32_t>(active.program_counter - 1),
    };
  }
  return storage.first(count);
}

std::vector<StackFrame> Execution::ResolveTrace(
    std::span<const runtime::TracePosition> positions) const {
  std::vector<StackFrame> trace;
  trace.reserve(positions.size());
  for (const auto position : positions) {
    const auto& function = functions_.subspan(position.function).front();
    const auto source_paths = function.source_paths != nullptr
                                  ? std::span<const std::string>(*function.source_paths)
                                  : std::span<const std::string>();
    const DebugInfo debug{
        .name = function.name,
        .source_paths = source_paths,
        .mappings = function.source_mappings,
    };
    StackFrame frame{
        .function = std::string(debug.name),
        .source_path = {},
        .location = SourceLocation(),
        .instruction = position.instruction,
    };
    if (const auto* mapping = debug.Locate(position.instruction)) {
      frame.location = mapping->location;
      frame.source_path = debug.source_paths.subspan(mapping->location.source_id).front();
    }
    trace.push_back(std::move(frame));
  }
  return trace;
}

std::vector<StackFrame> Execution::CaptureStack() const {
  std::array<runtime::TracePosition, kMaxCallFrames> positions;
  return ResolveTrace(CapturePositions(positions));
}

StepResult Execution::CreateException(runtime::Value payload,
                                      ExecutionStatus status,
                                      std::string_view message) {
  std::array<runtime::TracePosition, kMaxCallFrames> positions;
  auto* exception = isolate_.heap().AllocateException(*payload.AsObject(), status,
                                                      CapturePositions(positions), message);
  if (exception == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  pending_exception_ = runtime::Value::Object(exception);
  exception_root_.Attach(isolate_.heap(), &pending_exception_, 1);
  return {};
}

std::expected<runtime::Value, ExecutionStatus> Execution::RuntimeError(ExecutionStatus status,
                                                                       std::string_view message) {
  return runtime::CreateError(isolate_.heap(), isolate_.error_layout(),
                              {.message = message, .code = ExecutionStatusCode(status)});
}

void Execution::AbortInitializers() noexcept {
  if (globals_ == nullptr) {
    return;
  }
  for (std::size_t index = 0; index < frame_count_; ++index) {
    ResetInitializer(frames_.at(index));
  }
}

void Execution::ResetInitializer(ActiveFrame& active) noexcept {
  if (active.initialized_global) {
    std::span(globals_->states).subspan(*active.initialized_global).front() =
        GlobalState::Uninitialized;
    active.initialized_global.reset();
  }
}

std::optional<ExecutionResult> Execution::CompleteAsyncError(ActiveFrame& active) {
  const auto& exception = runtime::ExceptionData::From(*pending_exception_.AsObject());
  ExecutionResult failure{
      .value = runtime::Value::Object(exception.payload),
      .status = exception.status,
      .stack_trace = ResolveTrace(exception.trace()),
      .message = std::string(exception.message()),
  };
  auto diagnostic = exception.diagnostic != nullptr ? *exception.diagnostic : Diagnostic(failure);
  runtime::Future future(&isolate_.scheduler());
  const auto completion = future.CompleteError(failure.value, std::move(diagnostic));
  auto* object = completion ? isolate_.heap().AllocateFuture(std::move(future)) : nullptr;
  if (object == nullptr) {
    return ExecutionResult{
        .status = ExecutionStatus::OutOfMemory,
        .stack_trace = {},
        .message = {},
    };
  }
  pending_exception_ = runtime::Value::Null();
  const auto returned = Return(active, runtime::Value::Object(object));
  if (!returned) {
    return ExecutionResult{.status = returned.error(), .stack_trace = {}, .message = {}};
  }
  return std::nullopt;
}

std::optional<ExecutionResult> Execution::HandleError(ExecutionStatus status) {
  if (pending_exception_.IsNull()) {
    const auto message =
        error_message_.empty() ? ExecutionStatusName(status) : std::string_view(error_message_);
    const auto payload = RuntimeError(status, message);
    if (!payload || !CreateException(*payload, status, message)) {
      AbortInitializers();
      return ExecutionResult{
          .value = runtime::Value::Null(),
          .status = ExecutionStatus::OutOfMemory,
          .stack_trace = CaptureStack(),
          .message = {},
      };
    }
    error_message_.clear();
  }
  while (frame_count_ != 0) {
    auto& active = frames_.at(frame_count_ - 1);
    active.json_encoder.reset();
    active.dynamic_return = false;
    active.dynamic_call_plan = false;
    active.dynamic_assignment.reset();
    if (const auto* handler = FindHandler(active.function->handlers, active.program_counter - 1)) {
      active.program_counter = handler->target;
      return std::nullopt;
    }
    if (active.function->is_async && !IsResumedFrame()) {
      return CompleteAsyncError(active);
    }
    ResetInitializer(active);
    active.frame.Detach();
    --frame_count_;
  }
  const auto& exception = runtime::ExceptionData::From(*pending_exception_.AsObject());
  ExecutionResult failure{
      .value = runtime::Value::Object(exception.payload),
      .status = exception.status,
      .stack_trace = ResolveTrace(exception.trace()),
      .message = std::string(exception.message()),
  };
  if (const auto* diagnostic = exception.diagnostic) {
    failure.stack_trace.clear();
    for (const auto& frame : diagnostic->stack_trace) {
      failure.stack_trace.push_back({
          .function = frame.function,
          .source_path = frame.source_path,
          .location = {.line = frame.line, .column = frame.column},
          .instruction = frame.instruction,
      });
    }
  }
  return failure;
}

StepResult Execution::ExecuteException(ActiveFrame& active, Instruction instruction) {
  auto values = active.frame.registers();
  const auto opcode = DecodeOp(instruction);
  if (opcode == OpCode::Catch) {
    if (!IsException(pending_exception_)) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    values.subspan(DecodeA(instruction)).front() = pending_exception_;
    pending_exception_ = runtime::Value::Null();
    return {};
  }
  const auto value = values.subspan(DecodeB(instruction)).front();
  if (opcode == OpCode::ExceptionValue || opcode == OpCode::Rethrow) {
    if (!IsException(value)) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto& exception = runtime::ExceptionData::From(*value.AsObject());
    if (opcode == OpCode::ExceptionValue) {
      values.subspan(DecodeA(instruction)).front() = runtime::Value::Object(exception.payload);
      return {};
    }
    pending_exception_ = value;
    return std::unexpected(exception.status);
  }
  if (!runtime::Heap::IsThrowable(value.Read())) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto message = runtime::ErrorMessage(value, isolate_.error_layout());
  const auto created = CreateException(value, ExecutionStatus::Thrown, message);
  return std::unexpected(created ? ExecutionStatus::Thrown : created.error());
}

}  // namespace vela::vm::internal
