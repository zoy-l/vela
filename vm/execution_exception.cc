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
#include "runtime/string.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/exception_handler.h"
#include "vm/execution_status.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::vm::internal {
namespace {

auto FindHandler(std::span<const ExceptionHandler> handlers, std::size_t instruction)
    -> const ExceptionHandler* {
  const auto next = std::ranges::upper_bound(handlers, instruction, {}, &ExceptionHandler::begin);
  if (next == handlers.begin()) {
    return nullptr;
  }
  const auto& handler = *std::prev(next);
  return instruction < handler.end ? &handler : nullptr;
}

auto IsException(runtime::Value value) -> bool {
  return runtime::Heap::IsValueOfType(
      value, {.kind = runtime::ValueKind::Object, .object_kind = runtime::ObjectKind::Exception});
}

}  // namespace

auto Execution::CapturePositions(std::span<runtime::TracePosition> storage) const
    -> std::span<const runtime::TracePosition> {
  std::size_t count = 0;
  for (auto depth = frame_count_; depth != 0; --depth) {
    const auto& active = std::span(frames_).subspan(depth - 1).front();
    storage.subspan(count++).front() = {
        .function = static_cast<std::uint32_t>(active.function - functions_.data()),
        .instruction = static_cast<std::uint32_t>(active.program_counter - 1),
    };
  }
  return storage.first(count);
}

auto Execution::ResolveTrace(std::span<const runtime::TracePosition> positions) const
    -> std::vector<StackFrame> {
  std::vector<StackFrame> trace;
  trace.reserve(positions.size());
  for (const auto position : positions) {
    const auto debug = functions_.subspan(position.function).front().View().debug;
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

auto Execution::CaptureStack() const -> std::vector<StackFrame> {
  std::array<runtime::TracePosition, kMaxCallFrames> positions;
  return ResolveTrace(CapturePositions(positions));
}

auto Execution::CreateException(runtime::Value payload,
                                ExecutionStatus status,
                                std::string_view message) -> StepResult {
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

auto Execution::RuntimeError(ExecutionStatus status, std::string_view message)
    -> std::expected<runtime::Value, ExecutionStatus> {
  auto& heap = isolate_.heap();
  std::array<runtime::Value, runtime::kErrorFieldCount> fields;
  runtime::RootRange roots(heap, fields.data(), fields.size());
  auto* text = heap.AllocateString(message);
  if (text == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  fields.at(runtime::kErrorMessageField) = runtime::Value::String(text);
  auto* code = heap.AllocateString(ExecutionStatusName(status));
  if (code == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  fields.at(runtime::kErrorCodeField) = runtime::Value::String(code);
  auto* instance = heap.AllocateInstance(isolate_.error_layout(), fields);
  if (instance == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  return runtime::Value::Object(instance);
}

void Execution::AbortInitializers() noexcept {
  if (globals_ == nullptr) {
    return;
  }
  for (const auto& active : std::span(frames_).first(frame_count_)) {
    if (active.initialized_global) {
      globals_->states.at(*active.initialized_global) = GlobalState::Uninitialized;
    }
  }
}

auto Execution::HandleError(ExecutionStatus status) -> std::optional<ExecutionResult> {
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
    auto& active = std::span(frames_).subspan(frame_count_ - 1).front();
    if (const auto* handler = FindHandler(active.function->handlers, active.program_counter - 1)) {
      active.program_counter = handler->target;
      return std::nullopt;
    }
    if (active.initialized_global) {
      globals_->states.at(*active.initialized_global) = GlobalState::Uninitialized;
    }
    active.frame.Detach();
    --frame_count_;
  }
  const auto& exception = runtime::ExceptionData::From(*pending_exception_.AsObject());
  return ExecutionResult{
      .value = runtime::Value::Object(exception.payload),
      .status = exception.status,
      .stack_trace = ResolveTrace(exception.trace()),
      .message = std::string(exception.message()),
  };
}

auto Execution::ExecuteException(ActiveFrame& active, Instruction instruction) -> StepResult {
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
  if (!runtime::Heap::IsValueOfType(value, {.kind = runtime::ValueKind::Object})) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  std::string_view message;
  if (runtime::Heap::IsValueOfType(
          value, {.kind = runtime::ValueKind::Object, .instance = &isolate_.error_layout()})) {
    const auto* text = runtime::Heap::GetField(*value.AsObject(), runtime::kErrorMessageField);
    if (text != nullptr && text->IsString()) {
      message = text->AsString()->value();
    }
  }
  const auto created = CreateException(value, ExecutionStatus::Thrown, message);
  return std::unexpected(created ? ExecutionStatus::Thrown : created.error());
}

}  // namespace vela::vm::internal
