#include "runtime/async/future_composition.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "base/async_operation.h"
#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/async/future_operations.h"
#include "runtime/exceptions/error.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {
namespace {

using OperationResult = std::expected<void, ExecutionStatus>;

// timer/微任务拥有一份稳定值序列及其根，析构根后才释放被借用的容器。
class RootedValues final {
 public:
  RootedValues(Heap& heap, std::shared_ptr<std::vector<Value>> values)
      : values_(std::move(values)), roots_(heap, values_->data(), values_->size(), std::nullopt) {}

 private:
  std::shared_ptr<std::vector<Value>> values_;
  RootRange roots_;
};

void Forward(Future& target, const FutureResult& result) {
  std::ignore = target.Settle(result);
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
      heap.SetValueSlot(*storage, kValuesStart + count + 1 + index, retained[index]);
    }
    return storage;
  }
  Aggregation(Isolate& isolate,
              std::optional<FutureCallback> cleanup,
              Future result,
              std::shared_ptr<std::vector<Value>> references,
              const ArrayLayout* layout,
              std::size_t count,
              AsyncOperation operation)
      : isolate_(&isolate),
        cleanup_(std::move(cleanup)),
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
    if (!value.IsNull() && cleanup_) {
      const std::array<Value, 1> arguments{value};
      static_cast<void>(cleanup_->invoke(arguments));
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
          Clean(values[slot]);
          heap.SetValueSlot(storage, slot, Value::Null());
        }
        if (values[1].AsBoolean()) {
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
      CompleteFutureStatus(*isolate_, result_, ExecutionStatus::OutOfMemory);
      return;
    }
    const auto output = Value::Object(object);
    RootRange root(isolate_->heap(), &output, 1, std::nullopt);
    for (std::uint32_t index = 0; index < count_; ++index) {
      const auto value = UnboxDynamicValue(values[kValuesStart + index], layout_->element);
      const auto stored = isolate_->heap().SetElement(*object, index, value);
      if (!stored) {
        const auto status = stored.error() == ArrayAccessError::OutOfMemory
                                ? ExecutionStatus::OutOfMemory
                                : ExecutionStatus::TypeError;
        CompleteFutureStatus(*isolate_, result_, status);
        return;
      }
    }
    Forward(result_, {.state = FutureState::Succeeded, .value = output, .diagnostic = {}});
  }

  Isolate* isolate_;
  std::optional<FutureCallback> cleanup_;
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
  static constexpr std::size_t kCompletionReferences = 2;
  Isolate* isolate;
  Future* source;
  Future target;
  std::shared_ptr<std::vector<Value>> references;
  const TypeSignature* result_type = nullptr;
  FutureDiagnostic type_error;
  FutureDiagnostic timeout_error;
};

using Continuation = std::shared_ptr<const FutureContinuation>;

void CompleteCallback(const FutureContinuation& context, Future& source) {
  assert(context.result_type != nullptr);
  auto& isolate = *context.isolate;
  auto* object = isolate.heap().AllocateFuture(source);
  auto target = context.target;
  if (object == nullptr) {
    CompleteFutureStatus(isolate, target, ExecutionStatus::OutOfMemory);
    return;
  }
  const auto& retained = context.references;
  retained->push_back(Value::Object(object));
  target.KeepAlive(retained);
  ObserveCheckedCompletion(isolate, target, source, context.result_type->result, context.type_error,
                           retained);
}

OperationResult ScheduleTimeout(const Continuation& context,
                                const FutureCall& call,
                                const FutureCallbacks& callbacks) {
  const auto& runtime_type = call.runtime_type;
  if (!runtime_type) {
    std::unreachable();
  }
  auto* isolate = context->isolate;
  const auto& references = context->references;
  auto& heap = isolate->heap();
  auto& scheduler = isolate->scheduler();
  const auto delay = call.At(1);
  if (!delay.IsInteger() ||
      !FutureScheduler::CanSchedule(std::chrono::milliseconds(delay.AsInteger()))) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto timeout_type = *runtime_type;
  const auto* timeout_layout = timeout_type.nominal->fixed_layout();
  const auto callback = call.At(2);
  std::optional<FutureCallback> replacement;
  if (!callback.IsNull()) {
    auto resolved = callbacks.resolve(callback);
    if (!resolved || resolved->arity != 0) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    replacement = std::move(*resolved);
  }
  auto won = std::make_shared<bool>(false);
  auto roots = std::make_shared<RootedValues>(heap, references);
  auto timer = scheduler.After(
      std::chrono::milliseconds(delay.AsInteger()),
      [context, callback = std::move(replacement), won, roots, timeout_layout] {
        if (std::exchange(*won, true)) {
          return;
        }
        if (callback) {
          auto future = callback->invoke({});
          CompleteCallback(*context, future);
        } else {
          auto target = context->target;
          const auto error = CreateError(context->isolate->heap(), *timeout_layout,
                                         {.message = "Future timed out", .code = "Timeout"});
          Forward(target, {
                              .state = FutureState::Failed,
                              .value = error.value_or(Value::Null()),
                              .diagnostic = context->timeout_error,
                          });
        }
      });
  context->source->ObserveResult(
      [context, won, timer](const FutureResult& input) mutable {
        if (!std::exchange(*won, true)) {
          timer.Cancel();
          auto target = context->target;
          Forward(target, input);
        }
      },
      references);

  return {};
}

OperationResult ObserveCompletion(const Continuation& context,
                                  const FutureCall& call,
                                  const FutureCallbacks& callbacks) {
  auto action = callbacks.resolve(call.At(1));
  if (!action || action->arity != 0) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  context->source->ObserveResult(
      [context, action = std::move(*action)](const FutureResult& input) {
        auto& isolate = *context->isolate;
        auto target = context->target;
        auto completed = action.invoke({});
        const auto& retained = context->references;
        retained->push_back(input.value);
        auto* pending = isolate.heap().AllocateFuture(completed);
        if (pending == nullptr) {
          CompleteFutureStatus(isolate, target, ExecutionStatus::OutOfMemory);
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
      context->references);

  return {};
}

OperationResult ObserveCallbacks(const Continuation& context,
                                 const FutureCall& call,
                                 const FutureCallbacks& callbacks) {
  const auto& runtime_type = call.runtime_type;
  if (!runtime_type) {
    std::unreachable();
  }
  const auto operation = call.operation;

  const auto error_type = *runtime_type;
  const auto on_value = operation == AsyncOperation::Then ? call.At(1) : Value::Null();
  const auto on_error = operation == AsyncOperation::Then ? call.At(2) : call.At(1);
  std::optional<FutureCallback> value_callback;
  std::optional<FutureCallback> error_callback;
  if (operation == AsyncOperation::Then) {
    auto resolved = callbacks.resolve(on_value);
    if (!resolved || resolved->arity > 1) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    value_callback = std::move(*resolved);
  }
  if (!on_error.IsNull()) {
    auto resolved = callbacks.resolve(on_error);
    if (!resolved || resolved->arity != 1) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    error_callback = std::move(*resolved);
  }
  context->source->ObserveResult(
      [context, on_value = std::move(value_callback), on_error = std::move(error_callback),
       error_type](const FutureResult& input) {
        const auto& callback = input.state == FutureState::Succeeded ? on_value : on_error;
        if (!callback ||
            (input.state == FutureState::Failed && !Heap::IsValueOfType(input.value, error_type))) {
          auto target = context->target;
          Forward(target, input);
          return;
        }
        const std::array<Value, 1> argument_value{input.value};
        auto invoked = callback->invoke(std::span(argument_value).first(callback->arity));
        CompleteCallback(*context, invoked);
      },
      context->references);
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
  auto callback = callbacks.resolve(callable);
  if (!callback || callback->arity != 0) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto references = std::make_shared<std::vector<Value>>(1, callable);
  references->insert(references->end(), call.retained.begin(), call.retained.end());
  auto roots = std::make_shared<RootedValues>(heap, references);
  if (operation == AsyncOperation::TimerAfter || operation == AsyncOperation::TimerPeriodic) {
    auto* object = heap.AllocateTimer(Timer{});
    if (object == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    auto action = [callback = std::move(*callback), roots] {
      static_cast<void>(callback.invoke({}));
    };
    const auto duration = std::chrono::milliseconds(first.AsInteger());
    *Heap::GetTimer(*object) = operation == AsyncOperation::TimerAfter
                                   ? scheduler.After(duration, std::move(action))
                                   : scheduler.Periodic(duration, std::move(action));
    return Value::Object(object);
  }
  Future result(&scheduler);
  auto* object = heap.AllocateFuture(result);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  // 排队计算自行保根；丢弃脚本返回值不取消副作用，停止调度器则释放该内部根。
  auto action = [&isolate, callback = std::move(*callback), roots,
                 result = result.Retained()] mutable {
    const auto invoked = callback.invoke({});
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
  std::optional<FutureCallback> cleanup_callback;
  if (operation == AsyncOperation::Wait) {
    if (!argument(1).IsBoolean()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto cleanup = argument(2);
    if (!cleanup.IsNull()) {
      auto resolved = callbacks.resolve(cleanup);
      if (!resolved || resolved->arity != (layout != nullptr ? 1U : 0U)) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      cleanup_callback = std::move(*resolved);
    }
  }
  // 所有输入经同一个 GC 槽对象保留聚合状态，避免每个 waiter 重扫完整结果列表。
  auto* storage = Aggregation::AllocateStorage(heap, call, *length, call.retained);
  if (storage == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto owner = Value::Object(storage);
  RootRange owner_root(heap, &owner, 1, std::nullopt);
  Future result(&scheduler);
  auto* object = heap.AllocateFuture(result);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  heap.SetValueSlot(*storage, Aggregation::kResultSlot, Value::Object(object));
  auto references = std::make_shared<std::vector<Value>>(1, owner);
  result.KeepAlive(references);
  auto aggregate =
      std::make_shared<Aggregation>(isolate, std::move(cleanup_callback), result.Internal(),
                                    references, layout, *length, operation);
  aggregate->Start();
  return Value::Object(object);
}

std::expected<Value, ExecutionStatus> ExecuteComposition(Isolate& isolate,
                                                         FutureCall call,
                                                         const FutureCallbacks& callbacks) {
  const auto operation = call.operation;
  const auto arguments = call.values;
  const auto argument = [&call](std::size_t index) { return call.At(index); };
  auto& heap = isolate.heap();
  auto& scheduler = isolate.scheduler();
  const auto first = argument(0);
  auto* source = AsFuture(first);
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
  auto references = std::make_shared<std::vector<Value>>();
  // 每个续延最多追加原结果和待完成 Future；预留全部槽后，监听器借用的根地址保持稳定。
  references->reserve(arguments.size() + 1 + call.retained.size() +
                      FutureContinuation::kCompletionReferences);
  references->insert(references->end(), arguments.begin(), arguments.end());
  references->push_back(Value::Object(object));
  references->insert(references->end(), call.retained.begin(), call.retained.end());
  result.KeepAlive(references);
  auto target = result.Internal();

  const auto context = std::make_shared<const FutureContinuation>(FutureContinuation{
      .isolate = &isolate,
      .source = source,
      .target = std::move(target),
      .references = std::move(references),
      .result_type = result_type,
      .type_error = std::move(call.type_error),
      .timeout_error = std::move(call.timeout_error),
  });
  OperationResult observed;
  if (operation == AsyncOperation::Timeout) {
    observed = ScheduleTimeout(context, call, callbacks);
  } else if (operation == AsyncOperation::WhenComplete) {
    observed = ObserveCompletion(context, call, callbacks);
  } else {
    observed = ObserveCallbacks(context, call, callbacks);
  }
  if (!observed) {
    return std::unexpected(observed.error());
  }
  return Value::Object(object);
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteFutureOperation(Isolate& isolate,
                                                             FutureCall call,
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
      return ExecuteComposition(isolate, std::move(call), callbacks);
    default:
      std::unreachable();
  }
}

}  // namespace vela::runtime
