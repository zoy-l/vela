#include "runtime/native/native_abi.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/exceptions/error.h"
#include "runtime/exceptions/exception.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/native_image.h"
#include "runtime/value/value.h"
#include "vela/error.h"

namespace vela::runtime {
std::vector<FutureTraceFrame> NativeExecution::CaptureTrace() const {
  std::vector<FutureTraceFrame> trace;
  for (auto* frame = current_frame_; frame != nullptr; frame = frame->previous) {
    const auto& image = *frame->image;
    const auto position = frame->position;
    const auto& function = image.function(position.function);
    const auto location = function.locations.values().subspan(position.instruction).front();
    trace.push_back({
        .function = std::string(function.name.text()),
        .source_path =
            std::string(image.source_paths.values().subspan(location.source_id).front().text()),
        .line = location.line,
        .column = location.column,
        .instruction = position.instruction,
        .identity = function.identity,
    });
  }
  return trace;
}

FutureDiagnostic NativeExecution::Diagnostic(ExecutionStatus status,
                                             std::string_view message) const {
  return {
      .message = std::string(message),
      .stack_trace = CaptureTrace(),
      .status = status,
      .origin = {.context = isolate().scheduler().context(), .captured = true},
  };
}

FutureDiagnostic NativeExecution::ExceptionDiagnostic() const {
  const auto& exception = ExceptionData::From(*pending_exception_.AsObject());
  return *exception.diagnostic;
}

Value NativeExecution::ExceptionPayload() const noexcept {
  return HasException() ? Value::Object(ExceptionData::From(*pending_exception_.AsObject()).payload)
                        : Value::Null();
}

FutureDiagnostic NativeExecution::FailureDiagnostic(ExecutionStatus status) const {
  return HasException()
             ? ExceptionDiagnostic()
             : FutureDiagnostic{.message = {}, .stack_trace = {}, .status = status, .origin = {}};
}

void NativeExecution::AbortWithOutOfMemory() noexcept {
  exception_root_.Detach();
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
  ExceptionData::From(*pending_exception_.AsObject()).diagnostic = std::move(stored);
  return status;
}

ExecutionStatus NativeExecution::RestoreHostError(vela::Error error) {
  if (exception_allocation_failed_) {
    return ExecutionStatus::OutOfMemory;
  }
  const auto status = error.status;
  const auto payload = vela::internal::HandleAccess::ReadErrorPayload(error).value();
  std::unique_ptr<FutureDiagnostic> diagnostic(new (std::nothrow) FutureDiagnostic{
      .message = std::move(error.message),
      .stack_trace = std::move(error.stack_trace),
      .status = status,
      .origin = error.origin,
  });
  if (diagnostic == nullptr) {
    AbortWithOutOfMemory();
    return ExecutionStatus::OutOfMemory;
  }
  auto* exception =
      isolate().heap().AllocateException(*payload.AsObject(), status, diagnostic->message);
  if (exception == nullptr) {
    AbortWithOutOfMemory();
    return ExecutionStatus::OutOfMemory;
  }
  ExceptionData::From(*exception).diagnostic = std::move(diagnostic);
  SetException(Value::Object(exception));
  return status;
}

ExecutionStatus NativeExecution::CreateException(Value payload,
                                                 ExecutionStatus status,
                                                 std::string_view message) {
  std::unique_ptr<FutureDiagnostic> diagnostic(new (std::nothrow)
                                                   FutureDiagnostic(Diagnostic(status, message)));
  if (diagnostic == nullptr) {
    exception_allocation_failed_ = true;
    return ExecutionStatus::OutOfMemory;
  }
  auto* exception = isolate().heap().AllocateException(*payload.AsObject(), status, message);
  if (exception == nullptr) {
    exception_allocation_failed_ = true;
    return ExecutionStatus::OutOfMemory;
  }
  auto& envelope = ExceptionData::From(*exception);
  diagnostic->origin.identity = isolate().scheduler().NewErrorIdentity();
  envelope.diagnostic = std::move(diagnostic);
  SetException(Value::Object(exception));
  return status;
}

void NativeExecution::SetException(Value exception) noexcept {
  pending_exception_ = exception;
  // 新异常在下一次托管分配前发布为根；增量 GC 由 Attach 追踪当前封套。
  exception_root_.Attach(isolate().heap(), &pending_exception_, 1);
}

ExecutionStatus NativeExecution::HandleFailure(ExecutionStatus status, std::string_view message) {
  if (status == ExecutionStatus::ExecutionStopped) {
    return status;
  }
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
  SetException(exception);
  return ExceptionData::From(*exception.AsObject()).status;
}

Value NativeExecution::TakeException() noexcept {
  const auto exception = pending_exception_;
  exception_root_.Detach();
  pending_exception_ = Value::Null();
  return exception;
}

extern "C" std::uint8_t vela_native_v1_failure(NativeExecution* execution, std::uint8_t status) {
  return ToNativeStatus(execution->HandleFailure(static_cast<ExecutionStatus>(status)));
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
  return ToNativeStatus(execution->Throw(DecodeNativeValue(*payload)));
}

extern "C" std::uint8_t vela_native_v1_rethrow(NativeExecution* execution,
                                               const NativeValue* exception) noexcept {
  return ToNativeStatus(execution->Rethrow(DecodeNativeValue(*exception)));
}

}  // namespace vela::runtime
