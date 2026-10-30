#include "runtime/future_composition.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/async_operation.h"
#include "base/execution_status.h"
#include "runtime/error.h"
#include "runtime/future.h"
#include "runtime/future_operations.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

using OperationResult = std::expected<void, ExecutionStatus>;

void Forward(Future& target, const FutureResult& result) {
  std::ignore = result.state == FutureState::Succeeded
                    ? target.Complete(result.value)
                    : target.CompleteError(result.value, result.diagnostic);
}

class Aggregation final : public std::enable_shared_from_this<Aggregation> {
 public:
  static constexpr std::size_t kResultSlot = 4;
  static constexpr std::size_t kValuesStart = kResultSlot + 1;
  [[nodiscard]] static HeapObject* AllocateStorage(Heap& heap,
                                                   const FutureCall& call,
                                                   std::size_t count,
                                                   std::span<const Value> retained) {
    const auto slots = kValuesStart + count + 1 + retained.size();
    auto* storage = heap.AllocateValueSlots(slots);
    if (storage == nullptr) {
      return nullptr;
    }
    for (std::size_t index = 0; index < call.values.size(); ++index) {
      heap.SetValueSlot(*storage, index, call.At(index));
    }
    for (std::size_t index = 0; index < retained.size(); ++index) {
      heap.SetValueSlot(*storage, kValuesStart + count + 1 + index,
                        retained.subspan(index).front());
    }
    return storage;
  }
  Aggregation(Isolate& isolate,
              FutureCallbacks callbacks,
              Future result,
              std::shared_ptr<std::vector<Value>> references,
              const ArrayLayout* layout,
              std::size_t count,
              AsyncOperation operation)
      : isolate_(&isolate),
        callbacks_(std::move(callbacks)),
        result_(std::move(result)),
        references_(std::move(references)),
        storage_(references_->front()),
        layout_(layout),
        remaining_(count),
        count_(count),
        operation_(operation) {}

  void Start() {
    if (count_ == 0 && operation_ == AsyncOperation::Wait) {
      CompleteValues();
      return;
    }
    const auto values = Heap::ValueSlots(*storage_.AsObject());
    const auto source = values.front();
    for (std::uint32_t index = 0; index < count_; ++index) {
      const auto item = *Heap::GetElement(*source.AsObject(), index);
      auto* future = Heap::GetFuture(*item.AsObject());
      const auto self = shared_from_this();
      future->ObserveResult(
          [self, index](const FutureResult& result) { self->OnInput(index, result); }, references_);
    }
  }

 private:
  void Clean(Value value) {
    const auto values = Heap::ValueSlots(*storage_.AsObject());
    const auto callback = values.subspan(2).front();
    if (!value.IsNull() && !callback.IsNull()) {
      const std::array<Value, 1> arguments{value};
      static_cast<void>(callbacks_.invoke(callback, arguments));
    }
  }

  void OnInput(std::size_t index, const FutureResult& input) {
    auto& heap = isolate_->heap();
    auto& storage = *storage_.AsObject();
    const auto values = Heap::ValueSlots(storage);
    if (operation_ == AsyncOperation::Race) {
      if (result_.is_pending()) {
        Forward(result_, input);
      }
      return;
    }
    if (input.state == FutureState::Failed) {
      if (!failure_) {
        failure_ = input;
        heap.SetValueSlot(storage, kValuesStart + count_, input.value);
        for (std::size_t prior = 0; prior < count_; ++prior) {
          const auto slot = kValuesStart + prior;
          Clean(values.subspan(slot).front());
          heap.SetValueSlot(storage, slot, Value::Null());
        }
        if (values.subspan(1).front().AsBoolean()) {
          Forward(result_, input);
        }
      }
    } else if (failure_) {
      Clean(input.value);
    } else {
      heap.SetValueSlot(storage, kValuesStart + index, input.value);
    }
    if (--remaining_ == 0 && result_.is_pending()) {
      if (failure_) {
        Forward(result_, *failure_);
      } else {
        CompleteValues();
      }
    }
  }

  void CompleteValues() {
    const auto values = Heap::ValueSlots(*storage_.AsObject());
    if (layout_ == nullptr) {
      Forward(result_, {.state = FutureState::Succeeded, .value = Value::Null(), .diagnostic = {}});
      return;
    }
    auto* object = isolate_->heap().AllocateArray(*layout_, count_);
    if (object == nullptr) {
      const auto status = ExecutionStatus::OutOfMemory;
      const auto error = CreateError(
          isolate_->heap(), isolate_->error_layout(),
          {.message = ExecutionStatusName(status), .code = ExecutionStatusCode(status)});
      Forward(result_,
              {
                  .state = FutureState::Failed,
                  .value = error.value_or(Value::Null()),
                  .diagnostic = {.message = {}, .stack_trace = {}, .status = status, .origin = {}},
              });
      return;
    }
    const auto output = Value::Object(object);
    RootRange root(isolate_->heap(), &output, 1, RootOwner::Runtime);
    for (std::uint32_t index = 0; index < count_; ++index) {
      auto value = values.subspan(kValuesStart + index).front();
      const auto* instance = layout_->element.nominal;
      if (!value.IsNull() && instance != nullptr && instance->IsEnum()) {
        value = *Heap::GetField(*value.AsObject(), 0);
      }
      const auto stored = isolate_->heap().SetElement(*object, index, value);
      if (!stored) {
        const auto status = stored.error() == ArrayAccessError::OutOfMemory
                                ? ExecutionStatus::OutOfMemory
                                : ExecutionStatus::TypeError;
        CompleteFutureError(*isolate_, result_, Value::Null(),
                            {
                                .message = std::string(ExecutionStatusName(status)),
                                .stack_trace = {},
                                .status = status,
                                .origin = {},
                            });
        return;
      }
    }
    Forward(result_, {.state = FutureState::Succeeded, .value = output, .diagnostic = {}});
  }

  Isolate* isolate_;
  FutureCallbacks callbacks_;
  Future result_;
  std::shared_ptr<std::vector<Value>> references_;
  Value storage_;
  const ArrayLayout* layout_;
  std::size_t remaining_;
  std::size_t count_;
  AsyncOperation operation_;
  std::optional<FutureResult> failure_;
};

struct FutureContinuation final {
  Isolate* isolate;
  FutureCallbacks callbacks;
  Future* source;
  Future target;
  std::shared_ptr<std::vector<Value>> references;
  const TypeSignature* result_type = nullptr;
  FutureDiagnostic type_error;
};

void CompleteCallback(const FutureContinuation& context, Future& source) {
  assert(context.result_type != nullptr);
  auto& isolate = *context.isolate;
  auto* object = isolate.heap().AllocateFuture(source);
  auto target = context.target;
  if (object == nullptr) {
    CompleteFutureError(
        isolate, target, Value::Null(),
        {.message = {}, .stack_trace = {}, .status = ExecutionStatus::OutOfMemory, .origin = {}});
    return;
  }
  auto retained = std::make_shared<std::vector<Value>>(*context.references);
  retained->push_back(Value::Object(object));
  target.KeepAlive(retained);
  ObserveCheckedCompletion(isolate, target, source, context.result_type->result, context.type_error,
                           retained);
}

OperationResult ScheduleTimeout(const FutureContinuation& context,
                                const FutureCall& call,
                                const FutureDiagnostic& diagnostic) {
  auto* isolate = context.isolate;
  const auto callbacks = context.callbacks;
  auto* source = context.source;
  auto target = context.target;
  const auto references = context.references;
  auto& heap = isolate->heap();
  auto& scheduler = isolate->scheduler();
  const auto argument = [&call](std::size_t index) { return call.At(index); };

  const auto delay = argument(1);
  if (!delay.IsInteger() ||
      !FutureScheduler::CanSchedule(std::chrono::milliseconds(delay.AsInteger()))) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (!call.runtime_type) {
    std::unreachable();
  }
  const auto timeout_type = *call.runtime_type;
  const auto& error_layout = isolate->error_layout();
  const auto* timeout_layout =
      timeout_type.nominal == nullptr ? nullptr : timeout_type.nominal->fixed_layout();
  if (timeout_layout == nullptr || timeout_layout->field_count() != error_layout.field_count() ||
      !std::ranges::equal(timeout_layout->reference_fields(), error_layout.reference_fields())) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto callback = argument(2);
  if (!callback.IsNull()) {
    const auto arity = callbacks.arity(callback);
    if (!arity || *arity != 0) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  auto won = std::make_shared<bool>(false);
  auto roots =
      std::make_shared<RootRange>(heap, references->data(), references->size(), RootOwner::Runtime);
  auto timer = scheduler.After(std::chrono::milliseconds(delay.AsInteger()),
                               [context, isolate, callbacks, callback, target, won, references,
                                roots, timeout_layout, diagnostic]() mutable {
                                 if (std::exchange(*won, true)) {
                                   return;
                                 }
                                 if (!callback.IsNull()) {
                                   auto replacement = callbacks.invoke(callback, {});
                                   CompleteCallback(context, replacement);
                                 } else {
                                   const auto error = CreateError(
                                       isolate->heap(), *timeout_layout,
                                       {.message = "Future timed out", .code = "Timeout"});
                                   Forward(target, {
                                                       .state = FutureState::Failed,
                                                       .value = error.value_or(Value::Null()),
                                                       .diagnostic = diagnostic,
                                                   });
                                 }
                               });
  source->ObserveResult(
      [target, won, timer](const FutureResult& input) mutable {
        if (!std::exchange(*won, true)) {
          timer.Cancel();
          Forward(target, input);
        }
      },
      references);

  return {};
}

OperationResult ObserveCompletion(const FutureContinuation& context, const FutureCall& call) {
  auto* isolate = context.isolate;
  const auto callbacks = context.callbacks;
  auto* source = context.source;
  auto target = context.target;
  const auto references = context.references;
  const auto argument = [&call](std::size_t index) { return call.At(index); };

  const auto action = argument(1);
  const auto arity = callbacks.arity(action);
  if (!arity || *arity != 0) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  source->ObserveResult(
      [isolate, callbacks, target, action, references](const FutureResult& input) mutable {
        auto completed = callbacks.invoke(action, {});
        auto retained = std::make_shared<std::vector<Value>>(*references);
        retained->push_back(input.value);
        auto* pending = isolate->heap().AllocateFuture(completed);
        if (pending == nullptr) {
          CompleteFutureError(*isolate, target, Value::Null(),
                              {
                                  .message = {},
                                  .stack_trace = {},
                                  .status = ExecutionStatus::OutOfMemory,
                                  .origin = {},
                              });
          return;
        }
        retained->push_back(Value::Object(pending));
        target.KeepAlive(retained);
        completed.ObserveResult(
            [target, input](const FutureResult& final) mutable {
              Forward(target, final.state == FutureState::Failed ? final : input);
            },
            retained);
      },
      references);

  return {};
}

OperationResult ObserveCallbacks(const FutureContinuation& context, const FutureCall& call) {
  auto* isolate = context.isolate;
  const auto callbacks = context.callbacks;
  auto* source = context.source;
  auto target = context.target;
  const auto references = context.references;
  const auto operation = call.operation;
  const auto argument = [&call](std::size_t index) { return call.At(index); };

  if (!call.runtime_type) {
    std::unreachable();
  }
  const auto error_type = *call.runtime_type;
  const auto on_value = operation == AsyncOperation::Then ? argument(1) : Value::Null();
  const auto on_error = operation == AsyncOperation::Then ? argument(2) : argument(1);
  if (operation == AsyncOperation::Then) {
    const auto arity = callbacks.arity(on_value);
    if (!arity || *arity > 1) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  if (!on_error.IsNull()) {
    const auto arity = callbacks.arity(on_error);
    if (!arity || *arity != 1) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  source->ObserveResult(
      [context, isolate, callbacks, target, on_value, on_error,
       error_type](const FutureResult& input) mutable {
        const auto callback = input.state == FutureState::Succeeded ? on_value : on_error;
        if (callback.IsNull() ||
            (input.state == FutureState::Failed && !Heap::IsValueOfType(input.value, error_type))) {
          Forward(target, input);
          return;
        }
        const std::array<Value, 1> argument_value{input.value};
        const auto count = callbacks.arity(callback);
        if (!count || *count > 1) {
          CompleteFutureError(*isolate, target, Value::Null(),
                              {
                                  .message = {},
                                  .stack_trace = {},
                                  .status = ExecutionStatus::TypeError,
                                  .origin = {},
                              });
          return;
        }
        auto invoked = callbacks.invoke(callback, std::span(argument_value).first(*count));
        CompleteCallback(context, invoked);
      },
      references);
  return {};
}

std::expected<Value, ExecutionStatus> ExecuteComputation(Isolate& isolate,
                                                         const FutureCall& call,
                                                         const FutureCallbacks& callbacks) {
  const auto operation = call.operation;
  const auto argument = [&call](std::size_t index) { return call.At(index); };
  auto& heap = isolate.heap();
  auto& scheduler = isolate.scheduler();
  const auto first = argument(0);
  const auto delayed = operation == AsyncOperation::Delayed ||
                       operation == AsyncOperation::TimerAfter ||
                       operation == AsyncOperation::TimerPeriodic;

  if (delayed && (!first.IsInteger() ||
                  !FutureScheduler::CanSchedule(std::chrono::milliseconds(first.AsInteger())) ||
                  (operation == AsyncOperation::TimerPeriodic && first.AsInteger() == 0))) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto callable = argument(delayed ? 1 : 0);
  const auto arity = callbacks.arity(callable);
  if (!arity || *arity != 0) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto references = std::make_shared<std::vector<Value>>(1, callable);
  references->insert(references->end(), call.retained.begin(), call.retained.end());
  auto roots =
      std::make_shared<RootRange>(heap, references->data(), references->size(), RootOwner::Runtime);
  if (operation == AsyncOperation::TimerAfter || operation == AsyncOperation::TimerPeriodic) {
    auto action = [callbacks, callable, references, roots] {
      static_cast<void>(callbacks.invoke(callable, {}));
    };
    const auto duration = std::chrono::milliseconds(first.AsInteger());
    auto timer = operation == AsyncOperation::TimerAfter
                     ? scheduler.After(duration, std::move(action))
                     : scheduler.Periodic(duration, std::move(action));
    auto* object = heap.AllocateTimer(std::move(timer));
    return object != nullptr ? std::expected<Value, ExecutionStatus>(Value::Object(object))
                             : std::unexpected(ExecutionStatus::OutOfMemory);
  }
  Future result(&scheduler);
  auto* object = heap.AllocateFuture(result);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  // 排队计算自行保根；丢弃脚本返回值不取消副作用，停止调度器则释放该内部根。
  auto action = [&isolate, callbacks, callable, references, roots,
                 result = result.Retained()]() mutable {
    const auto invoked = callbacks.invoke(callable, {});
    CompleteFuture(isolate, result, invoked);
  };
  if (operation == AsyncOperation::Microtask) {
    scheduler.PostMicrotask(std::move(action));
  } else {
    static_cast<void>(
        scheduler.After(std::chrono::milliseconds(first.AsInteger()), std::move(action)));
  }
  return Value::Object(object);
}

std::expected<Value, ExecutionStatus> ExecuteAggregation(Isolate& isolate,
                                                         const FutureCall& call,
                                                         const FutureCallbacks& callbacks) {
  const auto operation = call.operation;
  const auto argument = [&call](std::size_t index) { return call.At(index); };
  auto& heap = isolate.heap();
  auto& scheduler = isolate.scheduler();
  const auto first = argument(0);

  const auto length = first.IsObject() ? Heap::ArrayLength(*first.AsObject())
                                       : std::unexpected(ArrayAccessError::NotArray);
  if (!length) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  for (std::uint32_t index = 0; index < *length; ++index) {
    const auto value = Heap::GetElement(*first.AsObject(), index);
    if (!value || !value->IsObject() || Heap::GetFuture(*value->AsObject()) == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  const auto* layout = call.wait_layout;
  if (operation == AsyncOperation::Wait) {
    if (!argument(1).IsBoolean()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto cleanup = argument(2);
    if (!cleanup.IsNull()) {
      const auto arity = callbacks.arity(cleanup);
      if (!arity || *arity != (layout != nullptr ? 1U : 0U)) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
    }
  }
  // 所有输入经同一个 GC 槽对象保留聚合状态，避免每个 waiter 重扫完整结果列表。
  auto* storage = Aggregation::AllocateStorage(heap, call, *length, call.retained);
  if (storage == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto owner = Value::Object(storage);
  RootRange owner_root(heap, &owner, 1, RootOwner::Runtime);
  Future result(&scheduler);
  auto* object = heap.AllocateFuture(result);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  heap.SetValueSlot(*storage, Aggregation::kResultSlot, Value::Object(object));
  auto references = std::make_shared<std::vector<Value>>(1, owner);
  result.KeepAlive(references);
  auto aggregate = std::make_shared<Aggregation>(isolate, callbacks, result.Internal(), references,
                                                 layout, *length, operation);
  aggregate->Start();
  return Value::Object(object);
}

std::expected<Value, ExecutionStatus> ExecuteComposition(Isolate& isolate,
                                                         const FutureCall& call,
                                                         const FutureCallbacks& callbacks) {
  const auto operation = call.operation;
  const auto arguments = call.values;
  const auto argument = [&call](std::size_t index) { return call.At(index); };
  auto& heap = isolate.heap();
  auto& scheduler = isolate.scheduler();
  const auto first = argument(0);
  auto* source = first.IsObject() ? Heap::GetFuture(*first.AsObject()) : nullptr;
  if (source == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  Future result(&scheduler);
  auto* object = heap.AllocateFuture(result);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto receiver_type = Heap::TypeOf(first);
  const TypeSignature* result_type = receiver_type.signature;
  if (operation == AsyncOperation::Then) {
    const auto callback = Heap::TypeOf(argument(1));
    if (!callback.callable || callback.signature == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    TypeSignature output;
    output.result = callback.signature->FlattenedResult();
    result_type = isolate.CreateTypeSignature(std::move(output));
  }
  if (result_type == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  std::ignore = Heap::BindRuntimeType(*object, {
                                                   .kind = ValueKind::Object,
                                                   .object_kind = ObjectKind::Future,
                                                   .signature = result_type,
                                               });
  auto references = std::make_shared<std::vector<Value>>(arguments.size() + 1);
  std::ranges::copy(arguments, references->begin());
  references->back() = Value::Object(object);
  references->insert(references->end(), call.retained.begin(), call.retained.end());
  result.KeepAlive(references);
  auto target = result.Internal();

  const FutureContinuation context{
      .isolate = &isolate,
      .callbacks = callbacks,
      .source = source,
      .target = target,
      .references = references,
      .result_type = result_type,
      .type_error = call.type_error,
  };
  OperationResult observed;
  if (operation == AsyncOperation::Timeout) {
    observed = ScheduleTimeout(context, call, call.timeout_error);
  } else if (operation == AsyncOperation::WhenComplete) {
    observed = ObserveCompletion(context, call);
  } else {
    observed = ObserveCallbacks(context, call);
  }
  if (!observed) {
    return std::unexpected(observed.error());
  }
  return Value::Object(object);
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteFutureOperation(Isolate& isolate,
                                                             const FutureCall& call,
                                                             const FutureCallbacks& callbacks) {
  switch (call.operation) {
    case AsyncOperation::Microtask:
    case AsyncOperation::Delayed:
    case AsyncOperation::TimerAfter:
    case AsyncOperation::TimerPeriodic:
      return ExecuteComputation(isolate, call, callbacks);
    case AsyncOperation::Wait:
    case AsyncOperation::Race:
      return ExecuteAggregation(isolate, call, callbacks);
    case AsyncOperation::Then:
    case AsyncOperation::CatchError:
    case AsyncOperation::WhenComplete:
    case AsyncOperation::Timeout:
      return ExecuteComposition(isolate, call, callbacks);
    default:
      std::unreachable();
  }
}

}  // namespace vela::runtime
