#include "runtime/native/native_async.h"
#include <optional>

#include <algorithm>
#include <array>
#include <bit>
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
#include "runtime/async/async_state.h"
#include "runtime/async/future.h"
#include "runtime/async/future_composition.h"
#include "runtime/async/future_operations.h"
#include "runtime/dynamic/dynamic_async.h"
#include "runtime/exceptions/error.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_version.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {

using internal::StoreNativeResult;

namespace {

enum class SuspensionReference : std::uint8_t { Instance, Output, Source, Count };
constexpr auto kSuspensionReferences = static_cast<std::size_t>(SuspensionReference::Count);

FutureCall DescribeFutureCall(NativeExecution& execution,
                              AsyncOperation operation,
                              std::span<const Value> values) {
  FutureCall call{
      .operation = operation,
      .values = values,
      .retained = execution.instance().state(),
      .type_error = {},
      .timeout_error = {},
  };
  if (operation == AsyncOperation::Then || operation == AsyncOperation::CatchError ||
      operation == AsyncOperation::Timeout) {
    call.type_error = execution.Diagnostic(ExecutionStatus::TypeError,
                                           "Future callback result does not match its result type");
  }
  if (operation == AsyncOperation::Timeout) {
    call.timeout_error = execution.Diagnostic(ExecutionStatus::Thrown, "Future timed out");
  }
  return call;
}

FutureDiagnostic CompletionDiagnostic(NativeExecution& execution,
                                      AsyncOperation operation,
                                      std::span<const Value> values) {
  return operation == AsyncOperation::CompleterError
             ? execution.Diagnostic(ExecutionStatus::Thrown,
                                    ErrorMessage(values[1], execution.isolate().error_layout()))
             : FutureDiagnostic{};
}

std::expected<Value, ExecutionStatus> BindResult(NativeExecution& execution,
                                                 std::uint32_t function,
                                                 Value value) {
  const auto& instance = execution.instance();
  const auto type = instance.image().function(function).result_type;
  return BindValueType(value, instance.layouts().types[type]);
}

std::expected<Value, ExecutionStatus> AllocateSuspensionResult(NativeExecution& execution,
                                                               std::uint32_t function) {
  auto& isolate = execution.isolate();
  auto& heap = isolate.heap();
  Future future(&isolate.scheduler());
  const auto allocated = AllocatedValue(heap.AllocateFuture(future));
  if (!allocated) {
    return allocated;
  }
  return BindResult(execution, function, *allocated);
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
        version_(*instance.AsObject(), function, std::nullopt, point.instruction) {
    Reference(SuspensionReference::Instance) = instance;
    Reference(SuspensionReference::Output) = output;
    isolate.scheduler().AddSuspendedFrame(SavedBytes());
  }
  ~NativeSuspendedFrame() { isolate_->scheduler().RemoveSuspendedFrame(SavedBytes()); }
  NativeSuspendedFrame(const NativeSuspendedFrame&) = delete;
  NativeSuspendedFrame& operator=(const NativeSuspendedFrame&) = delete;
  NativeSuspendedFrame(NativeSuspendedFrame&&) = delete;
  NativeSuspendedFrame& operator=(NativeSuspendedFrame&&) = delete;

  void Resume(const FutureResult& completion) {
    version_.SetExecuting();
    auto instance = NativeInstance::Borrow(*Reference(SuspensionReference::Instance).AsObject());
    NativeExecution execution(instance);
    NativeResumeState state{
        .saved = Saved().data(),
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
    CompleteFutureError(*isolate_, result(), execution.ExceptionPayload(),
                        execution.FailureDiagnostic(status));
  }

  void Suspend(std::uint32_t instruction, Value source, std::span<const NativeValue> saved) {
    auto future = RootFutureValue(*isolate_, source);
    point_.instruction = instruction;
    version_.SetSuspended(instruction);
    const auto destination = Saved();
    for (std::size_t index = 0; index < saved.size(); ++index) {
      destination[index] = DecodeNativeValue(saved[index]);
    }
    Reference(SuspensionReference::Source) = source;
    // 别名共享持有激活及其固定 vector，排队的 RootRange 始终指向同一存储。
    // 激活只保存 Future 的托管指针，不能反向持有强 Future 句柄形成 C++ 所有权环。
    std::shared_ptr<std::vector<Value>> references(shared_from_this(), &values_);
    result().KeepAlive(references);
    // references 已覆盖回调执行期，回调借用激活，无需重复持有同一个控制块。
    future.ObserveResult([this](const FutureResult& completion) { Resume(completion); },
                         std::move(references));
  }

  void ReleaseSaved() {
    std::ranges::fill(Saved(), Value::Null());
    Reference(SuspensionReference::Source) = Value::Null();
  }

  [[nodiscard]] Future& result() {
    return *Heap::GetFuture(*Reference(SuspensionReference::Output).AsObject());
  }

 private:
  [[nodiscard]] std::span<Value> Saved() noexcept {
    return std::span(values_).first(values_.size() - kSuspensionReferences);
  }
  [[nodiscard]] Value& Reference(SuspensionReference slot) noexcept {
    return std::span(values_).last<kSuspensionReferences>()[static_cast<std::size_t>(slot)];
  }
  [[nodiscard]] std::size_t SavedBytes() const noexcept {
    return sizeof(NativeSuspendedFrame) + (values_.capacity() * sizeof(Value));
  }
  Isolate* isolate_;
  SuspensionPoint point_;
  std::vector<Value> values_;
  NativeFunctionRoot version_;
};

namespace {

bool HasScalarAwaitStackBudget(std::uintptr_t stack_limit) {
  return std::bit_cast<std::uintptr_t>(&stack_limit) > stack_limit;
}

class ScalarAwaitReturn final {
 public:
  ScalarAwaitReturn(Isolate& isolate,
                    SuspensionPoint point,
                    std::uint32_t function,
                    std::array<Value, 2> references,
                    Value value,
                    std::uintptr_t stack_limit)
      : isolate_(&isolate),
        point_(point),
        function_(function),
        references_(references),
        value_(value),
        stack_limit_(stack_limit),
        roots_(isolate.heap(), references_.data(), references_.size(), std::nullopt) {
    isolate.scheduler().AddSuspendedFrame(sizeof(ScalarAwaitReturn));
  }
  ~ScalarAwaitReturn() { isolate_->scheduler().RemoveSuspendedFrame(sizeof(ScalarAwaitReturn)); }
  ScalarAwaitReturn(const ScalarAwaitReturn&) = delete;
  ScalarAwaitReturn& operator=(const ScalarAwaitReturn&) = delete;
  ScalarAwaitReturn(ScalarAwaitReturn&&) = delete;
  ScalarAwaitReturn& operator=(ScalarAwaitReturn&&) = delete;

  void Resume() {
    if (HasScalarAwaitStackBudget(stack_limit_)) {
      std::ignore = Heap::GetFuture(*references_[1].AsObject())->Complete(value_);
      return;
    }
    // 栈预算不足时由真实恢复入口建立原来的失败位置和诊断。
    const auto frame = std::make_shared<NativeSuspendedFrame>(*isolate_, point_, 0, references_[0],
                                                              function_, references_[1]);
    frame->Resume({.state = FutureState::Succeeded, .value = value_, .diagnostic = {}});
  }

 private:
  Isolate* isolate_;
  SuspensionPoint point_;
  std::uint32_t function_;
  std::array<Value, 2> references_;
  Value value_;
  std::uintptr_t stack_limit_;
  RootRange roots_;
};

std::expected<Value, ExecutionStatus> Suspend(NativeExecution& execution,
                                              std::uint32_t function,
                                              SuspensionPoint point,
                                              Value source,
                                              std::span<const NativeValue> saved,
                                              std::size_t capacity) {
  auto& isolate = execution.isolate();
  const auto allocated = AllocateSuspensionResult(execution, function);
  if (!allocated) {
    return allocated;
  }
  const auto output = *allocated;
  const RootRange root(isolate.heap(), &output, 1, std::nullopt);
  auto suspended = std::make_shared<NativeSuspendedFrame>(
      isolate, point, capacity, execution.instance().state().front(), function, output);
  suspended->Suspend(point.instruction, source, saved);
  return output;
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteNativeDynamicAsync(NativeExecution& execution,
                                                                std::span<const Value> arguments) {
  const auto selector = arguments[3];
  if (!selector.IsInteger() || selector.AsInteger() < 0 ||
      selector.AsInteger() >= static_cast<std::int64_t>(AsyncOperation::Count)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto operation = static_cast<AsyncOperation>(selector.AsInteger());
  const auto diagnostic = CompletionDiagnostic(execution, operation, arguments);
  const auto& instance = execution.instance();
  return ExecuteDynamicAsyncOperation(
      execution.isolate(), DescribeFutureCall(execution, operation, arguments),
      instance.layouts().types, NativeFutureCallbacks(instance), diagnostic);
}

extern "C" std::uint8_t vela_native_v1_new_future(NativeExecution* execution,
                                                  const NativeValue* payload,
                                                  std::uint8_t error,
                                                  NativeValue* result) {
  const auto value = DecodeNativeValue(*payload);
  auto& isolate = execution->isolate();
  return StoreNativeResult(
      result, error != 0 ? AllocateFutureError(
                               isolate, value,
                               execution->Diagnostic(ExecutionStatus::Thrown,
                                                     ErrorMessage(value, isolate.error_layout())))
                         : AllocateFutureValue(isolate, value));
}

extern "C" std::uint8_t vela_native_v1_sleep(NativeExecution* execution,
                                             std::int64_t milliseconds,
                                             NativeValue* result) {
  return StoreNativeResult(result, AllocateSleepFuture(execution->isolate(), milliseconds));
}

extern "C" std::uint8_t vela_native_v1_async_return(NativeExecution* execution,
                                                    std::uint32_t function,
                                                    const NativeValue* payload,
                                                    NativeValue* result) {
  const auto value = DecodeNativeValue(*payload);
  auto* nested = AsFuture(value);
  const auto future = nested != nullptr ? std::expected<Value, ExecutionStatus>(value)
                                        : AllocateFutureValue(execution->isolate(), value);
  return StoreNativeResult(result, future ? BindResult(*execution, function, *future) : future);
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
    decoded[index] = DecodeNativeValue(input[index]);
  }
  const auto diagnostic = CompletionDiagnostic(*execution, state, decoded);
  if (IsAsyncStateOperation(state)) {
    return StoreNativeResult(result,
                             ExecuteAsyncState(state, decoded, execution->isolate(), diagnostic));
  }
  const auto& instance = execution->instance();
  const auto& layouts = instance.layouts();
  auto call = DescribeFutureCall(*execution, state, decoded);
  if (state == AsyncOperation::Then || state == AsyncOperation::CatchError ||
      state == AsyncOperation::Timeout) {
    call.runtime_type = layouts.types[static_cast<std::size_t>(decoded.back().AsInteger())];
  }
  if (state == AsyncOperation::Wait) {
    const auto index = decoded.back().AsInteger();
    if (index >= 0) {
      call.wait_layout = layouts.arrays[static_cast<std::size_t>(index)];
    }
  }
  return StoreNativeResult(result, ExecuteFutureOperation(execution->isolate(), std::move(call),
                                                          NativeFutureCallbacks(instance)));
}

extern "C" std::uint8_t vela_native_v1_async_error(NativeExecution* execution,
                                                   std::uint32_t function,
                                                   NativeValue* result) {
  const auto future = AllocateFutureError(execution->isolate(), execution->ExceptionPayload(),
                                          execution->ExceptionDiagnostic());
  if (!future) {
    execution->AbortWithOutOfMemory();
    return ToNativeStatus(ExecutionStatus::OutOfMemory);
  }
  const auto bound = BindResult(*execution, function, *future);
  if (!bound) {
    return ToNativeStatus(bound.error());
  }
  std::ignore = execution->TakeException();
  return StoreNativeResult(result, bound);
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
  return StoreNativeResult(
      result, Suspend(*execution, function, {.entry = entry, .instruction = instruction},
                      DecodeNativeValue(*source), std::span(saved, count), capacity));
}

extern "C" std::uint8_t vela_native_v1_scalar_await_return(NativeExecution* execution,
                                                           std::uint32_t function,
                                                           NativeResumeEntry entry,
                                                           std::uint32_t instruction,
                                                           const NativeValue* source,
                                                           NativeValue* result) {
  auto& isolate = execution->isolate();
  const auto allocated = AllocateSuspensionResult(*execution, function);
  if (!allocated) {
    return ToNativeStatus(allocated.error());
  }
  const auto output = *allocated;
  auto frame = std::make_unique<ScalarAwaitReturn>(
      isolate, SuspensionPoint{.entry = entry, .instruction = instruction}, function,
      std::array{execution->instance().state().front(), output}, DecodeNativeValue(*source),
      execution->stack_limit());
  // 标量尾 await 仍经过一个相同的微任务边界；队列独占续延并持有其两个精确根。
  isolate.scheduler().PostMicrotask([frame = std::move(frame)] { frame->Resume(); });
  return StoreNativeResult(result, output);
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
  auto* nested = AsFuture(value);
  // 直接返回 Future 时仍验证绑定，并保留语言规定的 adoption 边界。
  if (nested != nullptr) {
    const auto bound = BindResult(*execution, function, value);
    if (!bound) {
      return ToNativeStatus(bound.error());
    }
    CompleteFuture(execution->isolate(), result, *nested);
  } else {
    std::ignore = result.Complete(value);
  }
  return ToNativeStatus(ExecutionStatus::Returned);
}

extern "C" std::uint8_t vela_native_v1_resume_outcome(NativeExecution* execution,
                                                      NativeResumeState* state,
                                                      NativeValue* result) {
  const auto& completion = *state->completion;
  state->frame->ReleaseSaved();
  return completion.state == FutureState::Succeeded
             ? StoreNativeResult(result, completion.value)
             : ToNativeStatus(execution->RestoreFutureError(completion));
}

}  // namespace vela::runtime
