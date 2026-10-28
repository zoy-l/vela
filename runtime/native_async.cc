#include "runtime/native_async.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
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
#include "runtime/value.h"
#include "runtime/value_operations.h"

namespace vela::runtime {

// 仅在一次恢复调用内借用；生成代码通过 ABI helper 读取，不依赖此 C++ 布局。
struct NativeResumeState final {
  std::span<Value> references;
  const FutureResult* completion = nullptr;
  std::uint32_t instruction = 0;
};

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

class NativeSuspendedFrame final {
 public:
  NativeSuspendedFrame(Isolate& isolate,
                       Future result,
                       SuspensionPoint point,
                       std::shared_ptr<std::vector<Value>> values)
      : isolate_(&isolate), result_(std::move(result)), point_(point), values_(std::move(values)) {
    isolate.scheduler().AddSuspendedFrame(SavedBytes());
  }
  ~NativeSuspendedFrame() { isolate_->scheduler().RemoveSuspendedFrame(SavedBytes()); }
  NativeSuspendedFrame(const NativeSuspendedFrame&) = delete;
  NativeSuspendedFrame& operator=(const NativeSuspendedFrame&) = delete;
  NativeSuspendedFrame(NativeSuspendedFrame&&) = delete;
  NativeSuspendedFrame& operator=(NativeSuspendedFrame&&) = delete;

  void Resume(const FutureResult& completion) {
    const auto values = std::span(*values_);
    const auto state_index = values.size() - kSuspensionReferences;
    auto instance = NativeInstance::Borrow(*values.subspan(state_index).front().AsObject());
    NativeExecution execution(instance);
    NativeResumeState state{
        .references = values,
        .completion = &completion,
        .instruction = point_.instruction,
    };
    HeapObject* result = nullptr;
    const auto status = static_cast<ExecutionStatus>(point_.entry(&execution, &state, &result));
    if (status == ExecutionStatus::Returned) {
      CompleteFuture(*isolate_, result_, *Heap::GetFuture(*result));
    } else if (execution.HasException()) {
      const auto& exception = ExceptionData::From(*execution.pending_exception().AsObject());
      CompleteFutureError(*isolate_, result_, Value::Object(exception.payload),
                          execution.ExceptionDiagnostic());
    } else {
      CompleteFutureError(*isolate_, result_, Value::Null(),
                          {.message = {}, .stack_trace = {}, .status = status});
    }
  }

 private:
  [[nodiscard]] std::size_t SavedBytes() const noexcept {
    return sizeof(NativeSuspendedFrame) + sizeof(std::vector<Value>) +
           (values_->capacity() * sizeof(Value));
  }
  Isolate* isolate_;
  Future result_;
  SuspensionPoint point_;
  std::shared_ptr<std::vector<Value>> values_;
};

std::expected<Value, ExecutionStatus> Suspend(NativeExecution& execution,
                                              std::uint32_t function,
                                              SuspensionPoint point,
                                              Value source,
                                              std::span<const NativeValue> saved) {
  auto& isolate = execution.isolate();
  auto* input = source.IsObject() ? Heap::GetFuture(*source.AsObject()) : nullptr;
  auto future = input != nullptr ? input->Rooted() : Future::Value(source, &isolate.scheduler());
  future.AttachHeap(isolate.heap());
  Future result(&isolate.scheduler());
  auto* object = isolate.heap().AllocateFuture(result);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto output = Value::Object(object);
  const RootRange root(isolate.heap(), &output, 1);
  auto references = std::make_shared<std::vector<Value>>();
  references->reserve(saved.size() + kSuspensionReferences);
  for (const auto& value : saved) {
    references->push_back(DecodeNativeValue(value));
  }
  references->push_back(execution.instance().state().front());
  references->push_back(output);
  references->push_back(source);
  std::shared_ptr<NativeSuspendedFrame> suspended(
      new (std::nothrow) NativeSuspendedFrame(isolate, result.Internal(), point, references));
  if (suspended == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto bound = BindResult(execution, function, output);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  result.KeepAlive(references);
  future.ObserveResult(
      [suspended](const FutureResult& completion) { suspended->Resume(completion); }, references);
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
                                               NativeValue* result) {
  return Store(result, Suspend(*execution, function, {.entry = entry, .instruction = instruction},
                               DecodeNativeValue(*source), std::span(saved, count)));
}

extern "C" std::uint32_t vela_native_v1_resume_instruction(NativeResumeState* state) noexcept {
  return state->instruction;
}

extern "C" void vela_native_v1_resume_value(NativeResumeState* state,
                                            std::uint64_t index,
                                            NativeValue* result) noexcept {
  *result = EncodeNativeValue(state->references.subspan(index).front());
}

extern "C" void vela_native_v1_resume_release(NativeResumeState* state) noexcept {
  const auto references = state->references;
  std::ranges::fill(references.first(references.size() - kSuspensionReferences), Value::Null());
  references.back() = Value::Null();
}

extern "C" std::uint8_t vela_native_v1_resume_outcome(NativeExecution* execution,
                                                      NativeResumeState* state,
                                                      NativeValue* result) {
  const auto& completion = *state->completion;
  return completion.state == FutureState::Succeeded
             ? Store(result, completion.value)
             : static_cast<std::uint8_t>(execution->RestoreFutureError(completion));
}

}  // namespace vela::runtime
