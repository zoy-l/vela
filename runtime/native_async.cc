#include "runtime/native_async.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "base/async_operation.h"
#include "base/execution_status.h"
#include "runtime/async_state.h"
#include "runtime/dynamic_async.h"
#include "runtime/error.h"
#include "runtime/exception.h"
#include "runtime/future.h"
#include "runtime/future_composition.h"
#include "runtime/future_operations.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_call.h"
#include "runtime/native_image.h"
#include "runtime/native_instance.h"
#include "runtime/native_version.h"
#include "runtime/value.h"
#include "runtime/value_operations.h"

namespace vela::runtime {

namespace {

constexpr std::size_t kSuspensionReferences = 3;

FutureCall DescribeFutureCall(NativeExecution& execution,
                              AsyncOperation operation,
                              std::span<const Value> values) {
  return {
      .operation = operation,
      .values = values,
      .retained = execution.instance().state(),
      .type_error = execution.Diagnostic(ExecutionStatus::TypeError,
                                         "Future callback result does not match its result type"),
      .timeout_error = execution.Diagnostic(ExecutionStatus::Thrown, "Future timed out"),
  };
}

std::uint8_t Store(NativeValue* result, std::expected<Value, ExecutionStatus> value) {
  if (!value) {
    return static_cast<std::uint8_t>(value.error());
  }
  *result = EncodeNativeValue(*value);
  return static_cast<std::uint8_t>(ExecutionStatus::Returned);
}

std::expected<Value, ExecutionStatus> BindResult(NativeExecution& execution,
                                                 std::uint32_t function,
                                                 Value value) {
  const auto& instance = execution.instance();
  const auto type = instance.image().functions.values().subspan(function).front().result_type;
  return BindValueType(value, instance.layouts().types.at(type));
}

struct SuspensionPoint final {
  NativeResumeEntry entry = nullptr;
  std::uint32_t instruction = 0;
};

}  // namespace

class NativeSuspendedFrame final : public std::enable_shared_from_this<NativeSuspendedFrame> {
 public:
  NativeSuspendedFrame(Isolate& isolate,
                       SuspensionPoint point,
                       std::size_t capacity,
                       Value instance,
                       std::uint32_t function,
                       Value output)
      : isolate_(&isolate),
        point_(point),
        values_(capacity + kSuspensionReferences),
        version_(*instance.AsObject(), function, RootOwner::Runtime, point.instruction) {
    values_.at(capacity) = instance;
    values_.at(capacity + 1) = output;
    isolate.scheduler().AddSuspendedFrame(SavedBytes());
  }
  ~NativeSuspendedFrame() { isolate_->scheduler().RemoveSuspendedFrame(SavedBytes()); }
  NativeSuspendedFrame(const NativeSuspendedFrame&) = delete;
  NativeSuspendedFrame& operator=(const NativeSuspendedFrame&) = delete;
  NativeSuspendedFrame(NativeSuspendedFrame&&) = delete;
  NativeSuspendedFrame& operator=(NativeSuspendedFrame&&) = delete;

  void Resume(const FutureResult& completion) {
    version_.SetExecuting();
    const auto values = std::span(values_);
    const auto state_index = values.size() - kSuspensionReferences;
    auto instance = NativeInstance::Borrow(*values.subspan(state_index).front().AsObject());
    NativeExecution execution(instance);
    NativeResumeState state{
        .saved = values.data(),
        .completion = &completion,
        .frame = this,
        .instruction = point_.instruction,
    };
    const auto status = static_cast<ExecutionStatus>(point_.entry(&execution, &state));
    if (status == ExecutionStatus::ExecutionStopped) {
      result().KeepAlive({});
      std::ranges::fill(values_, Value::Null());
      return;
    }
    if (status == ExecutionStatus::Returned) {
      return;
    }
    if (execution.HasException()) {
      const auto& exception = ExceptionData::From(*execution.pending_exception().AsObject());
      CompleteFutureError(*isolate_, result(), Value::Object(exception.payload),
                          execution.ExceptionDiagnostic());
    } else {
      CompleteFutureError(*isolate_, result(), Value::Null(),
                          {.message = {}, .stack_trace = {}, .status = status});
    }
  }

  void Suspend(std::uint32_t instruction, Value source, std::span<const NativeValue> saved) {
    auto* input = source.IsObject() ? Heap::GetFuture(*source.AsObject()) : nullptr;
    auto future =
        input != nullptr ? input->Rooted() : Future::Value(source, &isolate_->scheduler());
    future.AttachHeap(isolate_->heap());
    point_.instruction = instruction;
    version_.SetSuspended(instruction);
    for (std::size_t index = 0; index < saved.size(); ++index) {
      values_.at(index) = DecodeNativeValue(saved.subspan(index).front());
    }
    values_.back() = source;
    auto self = shared_from_this();
    // 别名共享持有激活及其固定 vector，排队的 RootRange 始终指向同一存储。
    // 激活只保存 Future 的托管指针，不能反向持有强 Future 句柄形成 C++ 所有权环。
    const std::shared_ptr<std::vector<Value>> references(self, &values_);
    result().KeepAlive(references);
    future.ObserveResult([self](const FutureResult& completion) { self->Resume(completion); },
                         references);
  }

  void ReleaseSaved() {
    const auto values = std::span(values_);
    std::ranges::fill(values.first(values.size() - kSuspensionReferences), Value::Null());
    values.back() = Value::Null();
  }

  [[nodiscard]] Future& result() {
    return *Heap::GetFuture(*values_.at(values_.size() - 2).AsObject());
  }

 private:
  [[nodiscard]] std::size_t SavedBytes() const noexcept {
    return sizeof(NativeSuspendedFrame) + (values_.capacity() * sizeof(Value));
  }
  Isolate* isolate_;
  SuspensionPoint point_;
  std::vector<Value> values_;
  NativeFunctionRoot version_;
};

namespace {

std::expected<Value, ExecutionStatus> Suspend(NativeExecution& execution,
                                              std::uint32_t function,
                                              SuspensionPoint point,
                                              Value source,
                                              std::span<const NativeValue> saved,
                                              std::size_t capacity) {
  auto& isolate = execution.isolate();
  Future result(&isolate.scheduler());
  auto* object = isolate.heap().AllocateFuture(result);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto output = Value::Object(object);
  const RootRange root(isolate.heap(), &output, 1, RootOwner::Runtime);
  auto suspended = std::make_shared<NativeSuspendedFrame>(
      isolate, point, capacity, execution.instance().state().front(), function, output);
  const auto bound = BindResult(execution, function, output);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  suspended->Suspend(point.instruction, source, saved);
  return output;
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteNativeDynamicAsync(NativeExecution& execution,
                                                                std::span<const Value> arguments) {
  const auto selector = arguments.subspan(3).front();
  FutureDiagnostic diagnostic;
  if (selector.IsInteger() &&
      selector.AsInteger() == static_cast<int>(AsyncOperation::CompleterError)) {
    diagnostic = execution.Diagnostic(
        ExecutionStatus::Thrown,
        ErrorMessage(arguments.subspan(1).front(), execution.isolate().error_layout()));
  }
  const auto& instance = execution.instance();
  return ExecuteDynamicAsyncOperation(
      execution.isolate(), DescribeFutureCall(execution, AsyncOperation::Count, arguments),
      instance.layouts().types, NativeFutureCallbacks(instance), diagnostic);
}

extern "C" std::uint8_t vela_native_v1_new_future(NativeExecution* execution,
                                                  const NativeValue* payload,
                                                  std::uint8_t error,
                                                  NativeValue* result) {
  const auto value = DecodeNativeValue(*payload);
  auto& isolate = execution->isolate();
  return Store(result, error != 0
                           ? AllocateFutureError(
                                 isolate, value,
                                 execution->Diagnostic(ExecutionStatus::Thrown,
                                                       ErrorMessage(value, isolate.error_layout())))
                           : AllocateFutureValue(isolate, value));
}

extern "C" std::uint8_t vela_native_v1_sleep(NativeExecution* execution,
                                             std::int64_t milliseconds,
                                             NativeValue* result) {
  return Store(result, AllocateSleepFuture(execution->isolate(), milliseconds));
}

extern "C" std::uint8_t vela_native_v1_async_return(NativeExecution* execution,
                                                    std::uint32_t function,
                                                    const NativeValue* payload,
                                                    NativeValue* result) {
  const auto value = DecodeNativeValue(*payload);
  auto* nested = value.IsObject() ? Heap::GetFuture(*value.AsObject()) : nullptr;
  const auto future = nested != nullptr ? std::expected<Value, ExecutionStatus>(value)
                                        : AllocateFutureValue(execution->isolate(), value);
  return Store(result, future ? BindResult(*execution, function, *future) : future);
}

extern "C" std::uint8_t vela_native_v1_async_call(NativeExecution* execution,
                                                  std::uint32_t operation,
                                                  const NativeValue* arguments,
                                                  NativeValue* result) {
  const auto state = static_cast<AsyncOperation>(operation);
  std::array<Value, kMaximumAsyncArity> storage;
  const auto decoded = std::span(storage).first(AsyncArity(state));
  const auto input = std::span(arguments, decoded.size());
  for (std::size_t index = 0; index < decoded.size(); ++index) {
    decoded.subspan(index).front() = DecodeNativeValue(input.subspan(index).front());
  }
  FutureDiagnostic diagnostic;
  if (state == AsyncOperation::CompleterError) {
    diagnostic = execution->Diagnostic(
        ExecutionStatus::Thrown,
        ErrorMessage(decoded.subspan(1).front(), execution->isolate().error_layout()));
  }
  if (IsAsyncStateOperation(state)) {
    return Store(result, ExecuteAsyncState(state, decoded, execution->isolate(), diagnostic));
  }
  const auto& instance = execution->instance();
  const auto& layouts = instance.layouts();
  auto call = DescribeFutureCall(*execution, state, decoded);
  if (state == AsyncOperation::Then || state == AsyncOperation::CatchError ||
      state == AsyncOperation::Timeout) {
    call.runtime_type = layouts.types.at(static_cast<std::size_t>(decoded.back().AsInteger()));
  }
  if (state == AsyncOperation::Wait) {
    const auto index = decoded.back().AsInteger();
    if (index >= 0) {
      call.wait_layout = layouts.arrays.at(static_cast<std::size_t>(index));
    }
  }
  return Store(result,
               ExecuteFutureOperation(execution->isolate(), call, NativeFutureCallbacks(instance)));
}

extern "C" std::uint8_t vela_native_v1_async_error(NativeExecution* execution,
                                                   std::uint32_t function,
                                                   NativeValue* result) {
  const auto& exception = ExceptionData::From(*execution->pending_exception().AsObject());
  const auto future = AllocateFutureError(execution->isolate(), Value::Object(exception.payload),
                                          execution->ExceptionDiagnostic());
  if (!future) {
    execution->AbortWithOutOfMemory();
    return static_cast<std::uint8_t>(ExecutionStatus::OutOfMemory);
  }
  const auto bound = BindResult(*execution, function, *future);
  if (!bound) {
    return static_cast<std::uint8_t>(bound.error());
  }
  std::ignore = execution->TakeException();
  return Store(result, bound);
}

extern "C" std::uint8_t vela_native_v1_suspend(NativeExecution* execution,
                                               std::uint32_t function,
                                               NativeResumeEntry entry,
                                               std::uint32_t instruction,
                                               const NativeValue* source,
                                               const NativeValue* saved,
                                               std::uint64_t count,
                                               std::uint64_t capacity,
                                               NativeValue* result) {
  return Store(result, Suspend(*execution, function, {.entry = entry, .instruction = instruction},
                               DecodeNativeValue(*source), std::span(saved, count), capacity));
}

extern "C" void vela_native_v1_resuspend(NativeResumeState* state,
                                         std::uint32_t instruction,
                                         const NativeValue* source,
                                         const NativeValue* saved,
                                         std::uint64_t count) {
  state->frame->Suspend(instruction, DecodeNativeValue(*source), std::span(saved, count));
}

extern "C" std::uint8_t vela_native_v1_resume_return(NativeExecution* execution,
                                                     NativeResumeState* state,
                                                     std::uint32_t function,
                                                     const NativeValue* payload) {
  const auto value = DecodeNativeValue(*payload);
  auto& result = state->frame->result();
  auto* nested = value.IsObject() ? Heap::GetFuture(*value.AsObject()) : nullptr;
  // 直接返回 Future 时仍验证绑定，并保留语言规定的 adoption 边界。
  if (nested != nullptr) {
    const auto bound = BindResult(*execution, function, value);
    if (!bound) {
      return static_cast<std::uint8_t>(bound.error());
    }
    CompleteFuture(execution->isolate(), result, *nested);
  } else {
    std::ignore = result.Complete(value);
  }
  return static_cast<std::uint8_t>(ExecutionStatus::Returned);
}

extern "C" std::uint8_t vela_native_v1_resume_outcome(NativeExecution* execution,
                                                      NativeResumeState* state,
                                                      NativeValue* result) {
  const auto& completion = *state->completion;
  state->frame->ReleaseSaved();
  return completion.state == FutureState::Succeeded
             ? Store(result, completion.value)
             : static_cast<std::uint8_t>(execution->RestoreFutureError(completion));
}

}  // namespace vela::runtime
