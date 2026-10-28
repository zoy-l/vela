#include "runtime/native_abi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "runtime/error.h"
#include "runtime/exception.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_instance.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

FutureDiagnostic ResolveTrace(const NativeImage& image,
                              std::span<const TracePosition> positions,
                              ExecutionStatus status,
                              std::string_view message) {
  FutureDiagnostic diagnostic{.message = std::string(message), .stack_trace = {}, .status = status};
  for (const auto position : positions) {
    const auto& function = image.functions.values().subspan(position.function).front();
    const auto location = function.locations.values().subspan(position.instruction).front();
    diagnostic.stack_trace.push_back({
        .function = std::string(function.name.text()),
        .source_path =
            std::string(image.source_paths.values().subspan(location.source_id).front().text()),
        .line = location.line,
        .column = location.column,
        .instruction = position.instruction,
    });
  }
  return diagnostic;
}

}  // namespace

void NativeExecution::SetInstruction(std::uint32_t instruction) noexcept {
  frames_.at(frame_count_ - 1)->set_instruction(instruction);
}

std::span<const TracePosition> NativeExecution::CapturePositions(
    std::span<TracePosition> storage) const noexcept {
  for (std::size_t index = 0; index < frame_count_; ++index) {
    storage.subspan(index).front() = frames_.at(frame_count_ - index - 1)->position();
  }
  return storage.first(frame_count_);
}

FutureDiagnostic NativeExecution::Diagnostic(ExecutionStatus status,
                                             std::string_view message) const {
  std::array<TracePosition, kMaxCallFrames> storage;
  return ResolveTrace(instance().image(), CapturePositions(storage), status, message);
}

FutureDiagnostic NativeExecution::ExceptionDiagnostic() const {
  const auto& exception = ExceptionData::From(*pending_exception_.AsObject());
  return exception.async_diagnostic != nullptr
             ? *exception.async_diagnostic
             : ResolveTrace(instance().image(), exception.trace(), exception.status,
                            exception.message());
}

void NativeExecution::AbortWithOutOfMemory() noexcept {
  pending_exception_ = Value::Null();
  exception_allocation_failed_ = true;
}

ExecutionStatus NativeExecution::RestoreFutureError(const FutureResult& completion) {
  const auto& diagnostic = completion.diagnostic;
  if (!completion.value.IsObject() || completion.value.AsObject() == nullptr) {
    return HandleFailure(diagnostic.status == ExecutionStatus::Thrown ? ExecutionStatus::TypeError
                                                                      : diagnostic.status,
                         diagnostic.message);
  }
  const auto status = CreateException(completion.value, diagnostic.status, diagnostic.message);
  if (!HasException()) {
    return status;
  }
  auto trace = diagnostic;
  auto boundary = Diagnostic(diagnostic.status, diagnostic.message);
  trace.stack_trace.insert(trace.stack_trace.end(),
                           std::make_move_iterator(boundary.stack_trace.begin()),
                           std::make_move_iterator(boundary.stack_trace.end()));
  std::unique_ptr<FutureDiagnostic> stored(new (std::nothrow) FutureDiagnostic(std::move(trace)));
  if (stored == nullptr) {
    AbortWithOutOfMemory();
    return ExecutionStatus::OutOfMemory;
  }
  ExceptionData::From(*pending_exception_.AsObject()).async_diagnostic = stored.release();
  return status;
}

ExecutionStatus NativeExecution::CreateException(Value payload,
                                                 ExecutionStatus status,
                                                 std::string_view message) {
  std::array<TracePosition, kMaxCallFrames> storage;
  auto* exception = isolate().heap().AllocateException(*payload.AsObject(), status,
                                                       CapturePositions(storage), message);
  if (exception == nullptr) {
    exception_allocation_failed_ = true;
    return ExecutionStatus::OutOfMemory;
  }
  pending_exception_ = Value::Object(exception);
  return status;
}

ExecutionStatus NativeExecution::HandleFailure(ExecutionStatus status, std::string_view message) {
  if (exception_allocation_failed_) {
    return ExecutionStatus::OutOfMemory;
  }
  if (HasException()) {
    return ExceptionData::From(*pending_exception_.AsObject()).status;
  }
  if (message.empty()) {
    message = ExecutionStatusName(status);
  }
  const auto error = CreateError(isolate().heap(), isolate().error_layout(),
                                 {.message = message, .code = ExecutionStatusCode(status)});
  if (!error) {
    exception_allocation_failed_ = true;
    return ExecutionStatus::OutOfMemory;
  }
  return CreateException(*error, status, message);
}

ExecutionStatus NativeExecution::Throw(Value payload) {
  if (!Heap::IsThrowable(payload)) {
    return ExecutionStatus::TypeError;
  }
  return CreateException(payload, ExecutionStatus::Thrown,
                         ErrorMessage(payload, isolate().error_layout()));
}

ExecutionStatus NativeExecution::Rethrow(Value exception) noexcept {
  pending_exception_ = exception;
  return ExceptionData::From(*exception.AsObject()).status;
}

Value NativeExecution::TakeException() noexcept {
  const auto exception = pending_exception_;
  pending_exception_ = Value::Null();
  return exception;
}

extern "C" void vela_native_v1_position(NativeExecution* execution,
                                        std::uint32_t instruction) noexcept {
  execution->SetInstruction(instruction);
}

extern "C" std::uint8_t vela_native_v1_failure(NativeExecution* execution, std::uint8_t status) {
  return static_cast<std::uint8_t>(execution->HandleFailure(static_cast<ExecutionStatus>(status)));
}

extern "C" std::uint8_t vela_native_v1_has_exception(NativeExecution* execution) noexcept {
  return execution->HasException() ? 1 : 0;
}

extern "C" void vela_native_v1_catch(NativeExecution* execution, NativeValue* result) noexcept {
  // 移交后到下一安全点之间不分配；生成代码负责在安全点前登记接收槽。
  *result = EncodeNativeValue(execution->TakeException());
}

extern "C" void vela_native_v1_exception_value(const NativeValue* exception,
                                               NativeValue* result) noexcept {
  const auto value = DecodeNativeValue(*exception);
  *result = EncodeNativeValue(Value::Object(ExceptionData::From(*value.AsObject()).payload));
}

extern "C" std::uint8_t vela_native_v1_throw(NativeExecution* execution,
                                             const NativeValue* payload) {
  return static_cast<std::uint8_t>(execution->Throw(DecodeNativeValue(*payload)));
}

extern "C" std::uint8_t vela_native_v1_rethrow(NativeExecution* execution,
                                               const NativeValue* exception) noexcept {
  return static_cast<std::uint8_t>(execution->Rethrow(DecodeNativeValue(*exception)));
}

}  // namespace vela::runtime
