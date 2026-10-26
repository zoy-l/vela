#include "base/callable.h"
#include "base/execution_status.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/execution.h"

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
#include <utility>
#include <vector>

#include "base/async_operation.h"
#include "runtime/error.h"
#include "vm/execution_status.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::vm::internal {
namespace {

using runtime::Future;
using runtime::FutureResult;
using runtime::FutureState;
using runtime::Value;

void Forward(Future& target, const FutureResult& result) {
  const auto completed = result.state == FutureState::Succeeded
                             ? target.Complete(result.value)
                             : target.CompleteError(result.value, result.diagnostic);
  // 只有获胜分支调用此函数；重复完成是内部调度错误，不是脚本输入检查。
  assert(completed.has_value());
  static_cast<void>(completed);
}

std::expected<std::size_t, ExecutionStatus> CallbackArity(const Program& program, Value callback) {
  if (!runtime::Heap::IsValueOfType(callback,
                                    {.kind = runtime::ValueKind::Object, .callable = true})) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* callable_layout = runtime::Heap::ClassOf(*callback.AsObject());
  if (!program.HasCallable(*callable_layout)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto target = *callable_layout->callable();
  auto function = target.function;
  if (target.mode == CallableMode::InterfaceMethod) {
    const auto receiver = *runtime::Heap::GetField(*callback.AsObject(), 0);
    const auto* layout =
        receiver.IsObject() ? runtime::Heap::ClassOf(*receiver.AsObject()) : nullptr;
    if (layout == nullptr || function >= layout->dispatch_targets().size()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    function = layout->dispatch_targets().subspan(function).front();
  }
  if (function >= program.functions().size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto hidden = target.mode == CallableMode::Plain ? 0U : 1U;
  return program.functions().subspan(function).front().parameter_count - hidden;
}

struct ScriptCallback final {
  ScriptCallback(runtime::Isolate& owner,
                 Program code,
                 Value function,
                 std::shared_ptr<DebugSession> session)
      : isolate(&owner),
        program(std::move(code)),
        callable(function),
        debug_session(std::move(session)) {}
  runtime::Isolate* isolate;
  Program program;
  Value callable;
  std::shared_ptr<DebugSession> debug_session;

  [[nodiscard]] Future Invoke(std::span<const Value> arguments = {}) const {
    return Execution::InvokeFuture(*isolate, program, callable, arguments, debug_session);
  }
};

class Aggregation final : public std::enable_shared_from_this<Aggregation> {
 public:
  static constexpr std::size_t kResultSlot = 4;
  static constexpr std::size_t kValuesStart = kResultSlot + 1;
  [[nodiscard]] static runtime::HeapObject* AllocateStorage(runtime::Heap& heap,
                                                            const AsyncArguments& call,
                                                            std::size_t count,
                                                            const GlobalStorage* globals) {
    const auto slots = kValuesStart + count + 1 + (globals != nullptr ? 1U : 0U);
    auto* storage = heap.AllocateValueSlots(slots);
    if (storage == nullptr) {
      return nullptr;
    }
    for (std::size_t index = 0; index < call.values.size(); ++index) {
      heap.SetValueSlot(*storage, index, call.At(index));
    }
    if (globals != nullptr) {
      heap.SetValueSlot(*storage, slots - 1, globals->storage);
    }
    return storage;
  }
  Aggregation(runtime::Isolate& isolate,
              Program program,
              Future result,
              std::shared_ptr<std::vector<Value>> references,
              const runtime::ArrayLayout* layout,
              std::size_t count,
              AsyncOperation operation,
              std::shared_ptr<DebugSession> debug_session)
      : isolate_(&isolate),
        program_(std::move(program)),
        result_(std::move(result)),
        references_(std::move(references)),
        storage_(references_->front()),
        layout_(layout),
        remaining_(count),
        count_(count),
        operation_(operation),
        debug_session_(std::move(debug_session)) {}

  void Start() {
    if (count_ == 0 && operation_ == AsyncOperation::Wait) {
      CompleteValues();
      return;
    }
    const auto values = runtime::Heap::ValueSlots(*storage_.AsObject());
    const auto source = values.front();
    for (std::uint32_t index = 0; index < count_; ++index) {
      const auto item = *runtime::Heap::GetElement(*source.AsObject(), index);
      auto* future = runtime::Heap::GetFuture(*item.AsObject());
      const auto self = shared_from_this();
      future->ObserveResult(
          [self, index](const FutureResult& result) { self->OnInput(index, result); }, references_);
    }
  }

 private:
  void Clean(Value value) {
    const auto values = runtime::Heap::ValueSlots(*storage_.AsObject());
    const auto callback = values.subspan(2).front();
    if (!value.IsNull() && !callback.IsNull()) {
      const std::array<Value, 1> arguments{value};
      static_cast<void>(
          ScriptCallback{*isolate_, program_, callback, debug_session_}.Invoke(arguments));
    }
  }

  void OnInput(std::size_t index, const FutureResult& input) {
    auto& heap = isolate_->heap();
    auto& storage = *storage_.AsObject();
    const auto values = runtime::Heap::ValueSlots(storage);
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
    const auto values = runtime::Heap::ValueSlots(*storage_.AsObject());
    if (layout_ == nullptr) {
      Forward(result_, {.state = FutureState::Succeeded, .value = Value::Null(), .diagnostic = {}});
      return;
    }
    auto* object = isolate_->heap().AllocateArray(*layout_, count_);
    if (object == nullptr) {
      const auto status = ExecutionStatus::OutOfMemory;
      const auto error = runtime::CreateError(
          isolate_->heap(), isolate_->error_layout(),
          {.message = ExecutionStatusName(status), .code = ExecutionStatusCode(status)});
      Forward(result_, {
                           .state = FutureState::Failed,
                           .value = error.value_or(Value::Null()),
                           .diagnostic = {.message = {}, .stack_trace = {}, .status = status},
                       });
      return;
    }
    const auto output = Value::Object(object);
    runtime::RootRange root(isolate_->heap(), &output, 1);
    for (std::uint32_t index = 0; index < count_; ++index) {
      auto value = values.subspan(kValuesStart + index).front();
      const auto* instance = layout_->element.instance;
      if (!value.IsNull() && instance != nullptr && instance->IsEnum()) {
        value = *runtime::Heap::GetField(*value.AsObject(), 0);
      }
      const auto stored = isolate_->heap().SetElement(*object, index, value);
      if (!stored) {
        const auto status = stored.error() == runtime::ArrayAccessError::OutOfMemory
                                ? ExecutionStatus::OutOfMemory
                                : ExecutionStatus::TypeError;
        Execution::CompleteFuture(*isolate_, result_,
                                  ExecutionResult{
                                      .status = status,
                                      .stack_trace = {},
                                      .message = std::string(ExecutionStatusName(status)),
                                  });
        return;
      }
    }
    Forward(result_, {.state = FutureState::Succeeded, .value = output, .diagnostic = {}});
  }

  runtime::Isolate* isolate_;
  Program program_;
  Future result_;
  std::shared_ptr<std::vector<Value>> references_;
  Value storage_;
  const runtime::ArrayLayout* layout_;
  std::size_t remaining_;
  std::size_t count_;
  AsyncOperation operation_;
  std::optional<FutureResult> failure_;
  std::shared_ptr<DebugSession> debug_session_;
};

struct FutureContinuation final {
  runtime::Isolate* isolate;
  Program program;
  std::shared_ptr<DebugSession> debug_session;
  Future* source;
  Future target;
  std::shared_ptr<std::vector<Value>> references;
  const runtime::TypeSignature* result_type = nullptr;
  runtime::FutureDiagnostic type_error;
};

void ObserveCheckedCompletion(const FutureContinuation& context,
                              Future& source,
                              std::optional<runtime::TypeConstraint> type,
                              const runtime::FutureDiagnostic& diagnostic) {
  auto* isolate = context.isolate;
  auto target = context.target;
  const auto references = context.references;
  source.ObserveResult(
      [isolate, target, type, diagnostic](const FutureResult& input) mutable {
        bool matches = runtime::Heap::IsThrowable(input.value);
        if (input.state == FutureState::Succeeded) {
          matches = type ? runtime::Heap::IsValueOfType(input.value, *type) : input.value.IsNull();
        }
        if (matches) {
          Forward(target, input);
        } else {
          const auto error =
              runtime::CreateError(isolate->heap(), isolate->error_layout(),
                                   {.message = diagnostic.message, .code = "TypeError"});
          Forward(target, {
                              .state = FutureState::Failed,
                              .value = error.value_or(Value::Null()),
                              .diagnostic = diagnostic,
                          });
        }
      },
      references);
}

StepResult ValidateNativeFuture(const FutureContinuation& context,
                                const AsyncArguments& call,
                                const runtime::FutureDiagnostic& diagnostic) {
  ObserveCheckedCompletion(context, *context.source, call.runtime_type, diagnostic);
  return {};
}

void CompleteCallback(const FutureContinuation& context, Future& source) {
  assert(context.result_type != nullptr);
  ObserveCheckedCompletion(context, source, context.result_type->result, context.type_error);
}

StepResult ScheduleTimeout(const FutureContinuation& context,
                           const AsyncArguments& call,
                           const runtime::FutureDiagnostic& diagnostic) {
  auto* isolate = context.isolate;
  const auto program = context.program;
  const auto debug_session = context.debug_session;
  auto* source = context.source;
  auto target = context.target;
  const auto references = context.references;
  auto& heap = isolate->heap();
  auto& scheduler = isolate->scheduler();
  const auto argument = [&call](std::size_t index) { return call.At(index); };

  const auto delay = argument(1);
  if (!delay.IsInteger() ||
      !runtime::FutureScheduler::CanSchedule(std::chrono::milliseconds(delay.AsInteger()))) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  assert(call.runtime_type.has_value());
  const auto timeout_type = *call.runtime_type;
  constexpr std::array<std::uint32_t, 2> kErrorReferences{
      runtime::kErrorMessageField,
      runtime::kErrorCodeField,
  };
  if (timeout_type.instance == nullptr ||
      timeout_type.instance->field_count() != runtime::kErrorFieldCount ||
      !std::ranges::equal(timeout_type.instance->reference_fields(), kErrorReferences)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto callback = argument(2);
  if (!callback.IsNull()) {
    const auto arity = CallbackArity(program, callback);
    if (!arity || *arity != 0) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  auto won = std::make_shared<bool>(false);
  auto roots = std::make_shared<runtime::RootRange>(heap, references->data(), references->size());
  auto timer = scheduler.After(
      std::chrono::milliseconds(delay.AsInteger()),
      [context, isolate, program, debug_session, callback, target, won, references, roots,
       timeout_type, diagnostic]() mutable {
        if (std::exchange(*won, true)) {
          return;
        }
        if (!callback.IsNull()) {
          auto replacement = ScriptCallback{*isolate, program, callback, debug_session}.Invoke();
          CompleteCallback(context, replacement);
        } else {
          const auto error =
              runtime::CreateError(isolate->heap(), *timeout_type.instance,
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

StepResult ObserveCompletion(const FutureContinuation& context, const AsyncArguments& call) {
  auto* isolate = context.isolate;
  const auto program = context.program;
  const auto debug_session = context.debug_session;
  auto* source = context.source;
  auto target = context.target;
  const auto references = context.references;
  const auto argument = [&call](std::size_t index) { return call.At(index); };

  const auto action = argument(1);
  const auto arity = CallbackArity(program, action);
  if (!arity || *arity != 0) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  source->ObserveResult(
      [isolate, program, debug_session, target, action,
       references](const FutureResult& input) mutable {
        auto completed = ScriptCallback{*isolate, program, action, debug_session}.Invoke();
        auto retained = std::make_shared<std::vector<Value>>(*references);
        retained->push_back(input.value);
        auto* pending = isolate->heap().AllocateFuture(completed);
        if (pending == nullptr) {
          Execution::CompleteFuture(*isolate, target,
                                    ExecutionResult{
                                        .status = ExecutionStatus::OutOfMemory,
                                        .stack_trace = {},
                                        .message = {},
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

StepResult ObserveCallbacks(const FutureContinuation& context, const AsyncArguments& call) {
  auto* isolate = context.isolate;
  const auto program = context.program;
  const auto debug_session = context.debug_session;
  auto* source = context.source;
  auto target = context.target;
  const auto references = context.references;
  const auto operation = call.operation;
  const auto argument = [&call](std::size_t index) { return call.At(index); };

  assert(call.runtime_type.has_value());
  const auto error_type = *call.runtime_type;
  const auto on_value = operation == AsyncOperation::Then ? argument(1) : Value::Null();
  const auto on_error = operation == AsyncOperation::Then ? argument(2) : argument(1);
  if (operation == AsyncOperation::Then) {
    const auto arity = CallbackArity(program, on_value);
    if (!arity || *arity > 1) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  if (!on_error.IsNull()) {
    const auto arity = CallbackArity(program, on_error);
    if (!arity || *arity != 1) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  source->ObserveResult(
      [context, isolate, program, debug_session, target, on_value, on_error,
       error_type](const FutureResult& input) mutable {
        const auto callback = input.state == FutureState::Succeeded ? on_value : on_error;
        if (callback.IsNull() || (input.state == FutureState::Failed &&
                                  !runtime::Heap::IsValueOfType(input.value, error_type))) {
          Forward(target, input);
          return;
        }
        const std::array<Value, 1> argument_value{input.value};
        const auto count = CallbackArity(program, callback);
        if (!count || *count > 1) {
          Execution::CompleteFuture(*isolate, target,
                                    ExecutionResult{
                                        .status = ExecutionStatus::TypeError,
                                        .stack_trace = {},
                                        .message = {},
                                    });
          return;
        }
        auto invoked = ScriptCallback{*isolate, program, callback, debug_session}.Invoke(
            std::span(argument_value).first(*count));
        CompleteCallback(context, invoked);
      },
      references);
  return {};
}

}  // namespace

StepResult Execution::ExecuteAsync(ActiveFrame& active, Instruction instruction) {
  const auto operation = static_cast<AsyncOperation>(DecodeB(instruction));
  const auto registers = active.frame.registers();
  AsyncArguments call{
      .operation = operation,
      .values = registers.subspan(DecodeC(instruction), AsyncArity(operation)),
      .destination = registers.subspan(DecodeA(instruction)).front(),
  };
  if (operation == AsyncOperation::Then || operation == AsyncOperation::CatchError ||
      operation == AsyncOperation::Timeout || operation == AsyncOperation::ValidateFuture) {
    const auto index = call.At(AsyncArity(operation) - 1);
    const auto types = active.function->layouts->types();
    if (!index.IsInteger() || index.AsInteger() < -1 ||
        (index.AsInteger() == -1 && operation != AsyncOperation::ValidateFuture) ||
        (index.AsInteger() >= 0 && static_cast<std::size_t>(index.AsInteger()) >= types.size())) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    if (index.AsInteger() >= 0) {
      call.runtime_type = types.subspan(static_cast<std::size_t>(index.AsInteger())).front();
    }
  }
  switch (operation) {
    case AsyncOperation::NewCompleter:
    case AsyncOperation::NewCancellationToken:
    case AsyncOperation::CancellationCancel:
    case AsyncOperation::CancellationIsCancelled:
    case AsyncOperation::TimerCancel:
    case AsyncOperation::CompleterFuture:
    case AsyncOperation::CompleterIsCompleted:
    case AsyncOperation::CompleterComplete:
    case AsyncOperation::CompleterError:
      return ExecuteAsyncState(call);
    case AsyncOperation::Microtask:
    case AsyncOperation::Delayed:
    case AsyncOperation::TimerAfter:
    case AsyncOperation::TimerPeriodic:
      return ExecuteAsyncComputation(call);
    case AsyncOperation::Wait:
    case AsyncOperation::Race:
      return ExecuteAsyncAggregation(active, call);
    default:
      return ExecuteAsyncComposition(call);
  }
}

StepResult Execution::ExecuteAsyncState(const AsyncArguments& call) {
  const auto operation = call.operation;
  auto destination = call.destination;
  const auto argument = [&call](std::size_t index) { return call.At(index); };
  const auto store_object = [&call](runtime::HeapObject* object) {
    return call.StoreObject(object);
  };
  auto& heap = isolate_.heap();
  auto& scheduler = isolate_.scheduler();
  if (operation == AsyncOperation::NewCompleter) {
    return store_object(heap.AllocateCompleter(Future(&scheduler)));
  }
  if (operation == AsyncOperation::NewCancellationToken) {
    return store_object(heap.AllocateCancellationToken(runtime::CancellationToken()));
  }
  const auto first = argument(0);
  if (operation == AsyncOperation::CancellationCancel ||
      operation == AsyncOperation::CancellationIsCancelled) {
    auto* token =
        first.IsObject() ? runtime::Heap::GetCancellationToken(*first.AsObject()) : nullptr;
    if (token == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    if (operation == AsyncOperation::CancellationCancel) {
      token->Cancel();
      destination.SetNull();
    } else {
      destination.SetBoolean(token->IsCancelled());
    }
    return {};
  }
  if (operation == AsyncOperation::TimerCancel) {
    auto* timer = first.IsObject() ? runtime::Heap::GetTimer(*first.AsObject()) : nullptr;
    if (timer == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    timer->Cancel();
    destination.SetNull();
    return {};
  }
  return ExecuteCompleter(call);

  std::unreachable();
}

StepResult Execution::ExecuteCompleter(const AsyncArguments& call) {
  const auto operation = call.operation;
  auto destination = call.destination;
  const auto argument = [&call](std::size_t index) { return call.At(index); };
  const auto first = argument(0);
  auto& heap = isolate_.heap();
  const auto store_object = [&call](runtime::HeapObject* object) {
    return call.StoreObject(object);
  };

  auto* completer = first.IsObject() ? runtime::Heap::GetCompleter(*first.AsObject()) : nullptr;
  if (completer == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (operation == AsyncOperation::CompleterFuture) {
    return store_object(heap.AllocateFuture(*completer));
  }
  if (operation == AsyncOperation::CompleterIsCompleted) {
    destination.SetBoolean(completer->IsCompleted());
    return {};
  }
  const auto value = argument(1);
  if (operation == AsyncOperation::CompleterError && !runtime::Heap::IsThrowable(value)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* adopted = value.IsObject() ? runtime::Heap::GetFuture(*value.AsObject()) : nullptr;
  const auto completion = [&] {
    if (operation == AsyncOperation::CompleterError) {
      return completer->CompleteError(value, ThrowDiagnostic(value));
    }
    if (adopted != nullptr) {
      return completer->Adopt(*adopted);
    }
    return completer->Complete(value);
  }();
  if (!completion) {
    return std::unexpected(runtime::FutureCompletionStatus(completion.error()));
  }
  destination.SetNull();
  return {};
}

StepResult Execution::ExecuteAsyncComputation(const AsyncArguments& call) {
  const auto operation = call.operation;
  auto destination = call.destination;
  const auto argument = [&call](std::size_t index) { return call.At(index); };
  const auto store_object = [&call](runtime::HeapObject* object) {
    return call.StoreObject(object);
  };
  auto& heap = isolate_.heap();
  auto& scheduler = isolate_.scheduler();
  const auto first = argument(0);
  const auto delayed = operation == AsyncOperation::Delayed ||
                       operation == AsyncOperation::TimerAfter ||
                       operation == AsyncOperation::TimerPeriodic;

  if (delayed &&
      (!first.IsInteger() ||
       !runtime::FutureScheduler::CanSchedule(std::chrono::milliseconds(first.AsInteger())) ||
       (operation == AsyncOperation::TimerPeriodic && first.AsInteger() == 0))) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto callable = argument(delayed ? 1 : 0);
  const auto arity = CallbackArity(program_, callable);
  if (!arity || *arity != 0) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto references = std::make_shared<std::vector<Value>>(1, callable);
  if (globals_ != nullptr) {
    references->push_back(globals_->storage);
  }
  auto roots = std::make_shared<runtime::RootRange>(heap, references->data(), references->size());
  const ScriptCallback callback{isolate_, program_.Internal(), callable, debug_session_};
  if (operation == AsyncOperation::TimerAfter || operation == AsyncOperation::TimerPeriodic) {
    auto action = [callback, references, roots] { static_cast<void>(callback.Invoke()); };
    const auto duration = std::chrono::milliseconds(first.AsInteger());
    auto timer = operation == AsyncOperation::TimerAfter
                     ? scheduler.After(duration, std::move(action))
                     : scheduler.Periodic(duration, std::move(action));
    return store_object(heap.AllocateTimer(std::move(timer)));
  }
  Future result(&scheduler);
  auto* object = heap.AllocateFuture(result);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  // 排队的计算拥有外部句柄；丢弃脚本返回值不会取消已排队副作用。
  auto action = [callback, references, roots, result]() mutable {
    const auto invoked = callback.Invoke();
    Execution::CompleteFuture(*callback.isolate, result, invoked);
  };
  if (operation == AsyncOperation::Microtask) {
    scheduler.PostMicrotask(std::move(action));
  } else {
    static_cast<void>(
        scheduler.After(std::chrono::milliseconds(first.AsInteger()), std::move(action)));
  }
  destination.SetObject(object);
  return {};
}

StepResult Execution::ExecuteAsyncAggregation(ActiveFrame& active, const AsyncArguments& call) {
  const auto operation = call.operation;
  auto destination = call.destination;
  const auto argument = [&call](std::size_t index) { return call.At(index); };
  auto& heap = isolate_.heap();
  auto& scheduler = isolate_.scheduler();
  const auto first = argument(0);

  const auto length = first.IsObject() ? runtime::Heap::ArrayLength(*first.AsObject())
                                       : std::unexpected(runtime::ArrayAccessError::NotArray);
  if (!length) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  for (std::uint32_t index = 0; index < *length; ++index) {
    const auto value = runtime::Heap::GetElement(*first.AsObject(), index);
    if (!value || !value->IsObject() || runtime::Heap::GetFuture(*value->AsObject()) == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  const runtime::ArrayLayout* layout = nullptr;
  if (operation == AsyncOperation::Wait) {
    const auto layout_index = argument(3);
    if (!argument(1).IsBoolean() || !layout_index.IsInteger() || layout_index.AsInteger() < -1 ||
        (layout_index.AsInteger() >= 0 && static_cast<std::size_t>(layout_index.AsInteger()) >=
                                              active.function->layouts->arrays().size())) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    if (layout_index.AsInteger() >= 0) {
      layout = active.function->layouts->arrays()
                   .subspan(static_cast<std::size_t>(layout_index.AsInteger()))
                   .front();
    }
    const auto cleanup = argument(2);
    if (!cleanup.IsNull()) {
      const auto arity = CallbackArity(program_, cleanup);
      if (!arity || *arity != (layout != nullptr ? 1U : 0U)) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
    }
  }
  // 所有输入经同一个 GC 槽对象保留聚合状态，避免每个 waiter 重扫完整结果列表。
  auto* storage = Aggregation::AllocateStorage(heap, call, *length, globals_);
  if (storage == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto owner = Value::Object(storage);
  runtime::RootRange owner_root(heap, &owner, 1);
  Future result(&scheduler);
  auto* object = heap.AllocateFuture(result);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  heap.SetValueSlot(*storage, Aggregation::kResultSlot, Value::Object(object));
  auto references = std::make_shared<std::vector<Value>>(1, owner);
  result.KeepAlive(references);
  auto aggregate =
      std::make_shared<Aggregation>(isolate_, program_.Internal(), result.Internal(), references,
                                    layout, *length, operation, debug_session_);
  aggregate->Start();
  destination.SetObject(object);
  return {};
}

StepResult Execution::ExecuteAsyncComposition(const AsyncArguments& call) {
  const auto operation = call.operation;
  const auto arguments = call.values;
  auto destination = call.destination;
  const auto argument = [&call](std::size_t index) { return call.At(index); };
  auto& heap = isolate_.heap();
  auto& scheduler = isolate_.scheduler();
  const auto first = argument(0);
  auto* source = first.IsObject() ? runtime::Heap::GetFuture(*first.AsObject()) : nullptr;
  if (source == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  Future result(&scheduler);
  auto* object = heap.AllocateFuture(result);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const runtime::TypeSignature* result_type = nullptr;
  if (operation != AsyncOperation::ValidateFuture) {
    const auto receiver_type = runtime::Heap::TypeOf(first);
    result_type = receiver_type.signature;
    if (operation == AsyncOperation::Then) {
      const auto callback = runtime::Heap::TypeOf(argument(1));
      if (!callback.callable || callback.signature == nullptr) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      runtime::TypeSignature output;
      output.result = callback.signature->FlattenedResult();
      result_type = isolate_.CreateTypeSignature(std::move(output));
    }
    if (result_type == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto* layout = isolate_.CreateArrayLayout({
        .element =
            {
                .kind = runtime::ValueKind::Object,
                .object_kind = runtime::ObjectKind::Future,
                .signature = result_type,
            },
    });
    const auto bound = runtime::Heap::BindRuntimeType(*object, *layout);
    assert(bound.has_value());
  }
  auto references = std::make_shared<std::vector<Value>>(arguments.size() + 1);
  arguments.CopyTo(std::span(*references).first(arguments.size()));
  references->back() = Value::Object(object);
  if (globals_ != nullptr) {
    references->push_back(globals_->storage);
  }
  result.KeepAlive(references);
  auto target = result.Internal();
  const auto program = program_.Internal();
  const auto debug_session = debug_session_;
  auto* isolate = &isolate_;
  const FutureContinuation context{
      .isolate = isolate,
      .program = program,
      .debug_session = debug_session,
      .source = source,
      .target = target,
      .references = references,
      .result_type = result_type,
      .type_error = Diagnostic({
          .status = ExecutionStatus::TypeError,
          .stack_trace = CaptureStack(),
          .message = "Future callback result does not match its result type",
      }),
  };
  StepResult observed;
  if (operation == AsyncOperation::ValidateFuture) {
    const auto diagnostic = Diagnostic({
        .status = ExecutionStatus::TypeError,
        .stack_trace = CaptureStack(),
        .message = "native Future completion does not match its external declaration",
    });
    observed = ValidateNativeFuture(context, call, diagnostic);
  } else if (operation == AsyncOperation::Timeout) {
    const auto diagnostic = Diagnostic({
        .status = ExecutionStatus::Thrown,
        .stack_trace = CaptureStack(),
        .message = "Future timed out",
    });
    observed = ScheduleTimeout(context, call, diagnostic);
  } else if (operation == AsyncOperation::WhenComplete) {
    observed = ObserveCompletion(context, call);
  } else {
    observed = ObserveCallbacks(context, call);
  }
  if (!observed) {
    return observed;
  }
  destination.SetObject(object);
  return {};
}

}  // namespace vela::vm::internal
