#include "runtime/native_call.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "base/callable.h"
#include "base/execution_status.h"
#include "runtime/callable.h"
#include "runtime/exception.h"
#include "runtime/future.h"
#include "runtime/future_composition.h"
#include "runtime/future_operations.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_dispatch.h"
#include "runtime/native_image.h"
#include "runtime/native_instance.h"
#include "runtime/native_program_state.h"
#include "runtime/object.h"
#include "runtime/value.h"
#include "runtime/value_operations.h"

namespace vela::runtime {
namespace {

struct NativeCallback final {
  HeapObject* environment = nullptr;
  ResolvedCallable target;
  const NativeFunctionDescription* function = nullptr;
};

struct NativeCallDomain final {
  Isolate* isolate;
  const CodeIdentity* identity;
  const NativeDispatch* dispatch;
};

NativeCallDomain Domain(const NativeInstance& instance) {
  return {
      .isolate = &instance.isolate(),
      .identity = &instance.code_identity(),
      .dispatch = Heap::NativeProgram(*instance.state().front().AsObject()).dispatch,
  };
}

std::expected<NativeCallback, ExecutionStatus> ResolveNativeCallback(const NativeCallDomain& domain,
                                                                     Value callable) {
  auto* object = callable.IsObject() ? callable.AsObject() : nullptr;
  auto* environment = object != nullptr ? Heap::NativeEnvironment(*object) : nullptr;
  if (environment == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& state = Heap::NativeProgram(*environment);
  if (state.isolate != domain.isolate) {
    return std::unexpected(ExecutionStatus::InvalidIsolate);
  }
  if (domain.dispatch != nullptr ? state.dispatch != domain.dispatch
                                 : state.code_identity != domain.identity) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto functions = state.image->functions.values();
  std::expected<ResolvedCallable, ExecutionStatus> target;
  if (state.dispatch == nullptr) {
    target = ResolveCallable(callable, *state.code_identity, functions.size());
  } else {
    ResolvedCallable selected{.function = Heap::NativeFunctionIndex(*object)};
    switch (Heap::ClassOf(*object)->callable()->mode) {
      case CallableMode::Plain:
        break;
      case CallableMode::Closure:
        selected.implicit = callable;
        break;
      case CallableMode::BoundMethod:
      case CallableMode::InterfaceMethod:
        selected.implicit = *Heap::GetField(*object, 0);
        break;
    }
    target = selected;
  }
  if (!target) {
    return std::unexpected(target.error());
  }
  return NativeCallback{
      .environment = environment,
      .target = *target,
      .function = &functions.subspan(target->function).front(),
  };
}

Future FailedCallback(Isolate& isolate, ExecutionStatus status) {
  Future result(&isolate.scheduler());
  result.AttachHeap(isolate.heap());
  CompleteFutureError(isolate, result, Value::Null(),
                      {.message = {}, .stack_trace = {}, .status = status});
  return result;
}

std::expected<Value, ExecutionStatus> InvokeCallback(NativeExecution& execution,
                                                     const NativeCallback& callback,
                                                     std::span<const Value> arguments) {
  const auto& function = *callback.function;
  const auto& target = callback.target;
  const auto hidden = target.implicit ? 1U : 0U;
  if (function.parameter_count != arguments.size() + hidden) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& types = execution.instance().layouts().types;
  const auto typed = (function.flags & kNativeTypedFunction) != 0;
  const auto parameters = function.parameters.values();
  std::vector<NativeValue> input(function.parameter_count);
  if (target.implicit) {
    input.front() = EncodeNativeValue(*target.implicit);
  }
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    auto value = arguments.subspan(index).front();
    if (typed) {
      const auto parameter =
          parameters.subspan(index + hidden - function.hidden_parameters).front();
      const auto& type = types.at(parameter.type);
      if (!Heap::IsValueOfType(value, type)) {
        return std::unexpected(ExecutionStatus::InvalidCast);
      }
      if (!value.IsNull() && type.nominal != nullptr && type.nominal->IsEnum()) {
        value = *Heap::GetField(*value.AsObject(), 0);
      }
    }
    input.at(index + hidden) = EncodeNativeValue(value);
  }
  return InvokeNativeTarget(execution, target.function, input);
}

Future InvokeFutureCallback(const NativeCallDomain& domain,
                            Value callable,
                            std::span<const Value> arguments) {
  auto& isolate = *domain.isolate;
  const RootRange callable_root(isolate.heap(), &callable, 1, RootOwner::Runtime);
  const RootRange arguments_root(isolate.heap(), arguments.data(), arguments.size(),
                                 RootOwner::Runtime);
  const auto callback = ResolveNativeCallback(domain, callable);
  if (!callback) {
    return FailedCallback(isolate, callback.error());
  }
  auto instance = NativeInstance::Borrow(*callback->environment);
  NativeExecution execution(instance);
  const auto invoked = InvokeCallback(execution, *callback, arguments);
  if (invoked) {
    const auto value = *invoked;
    auto* nested = value.IsObject() ? Heap::GetFuture(*value.AsObject()) : nullptr;
    if (nested != nullptr) {
      return nested->Rooted();
    }
    auto result = Future::Value(value, &isolate.scheduler());
    result.AttachHeap(isolate.heap());
    return result;
  }
  const auto status = execution.HandleFailure(invoked.error());
  if (!execution.HasException()) {
    return FailedCallback(isolate, status);
  }
  Future result(&isolate.scheduler());
  result.AttachHeap(isolate.heap());
  const auto& exception = ExceptionData::From(*execution.pending_exception().AsObject());
  CompleteFutureError(isolate, result, Value::Object(exception.payload),
                      execution.ExceptionDiagnostic());
  return result;
}

}  // namespace

std::expected<HeapObject*, ExecutionStatus> AllocateNativeCallable(
    NativeExecution& execution,
    const ClassLayout& layout,
    std::span<const Value> captures) {
  const auto target = *layout.callable();
  NativeResolvedDeclaration selected{
      .state = execution.instance().state().front().AsObject(),
      .function = target.function,
  };
  const auto& state = Heap::NativeProgram(*selected.state);
  if (state.dispatch != nullptr && target.mode != CallableMode::Closure &&
      target.mode != CallableMode::InterfaceMethod) {
    const auto identity = state.image->functions.values().subspan(target.function).front().identity;
    if (identity.local == 0) {
      const auto current = state.dispatch->Resolve(identity.declaration);
      if (!current) {
        return std::unexpected(current.error());
      }
      selected = *current;
    }
  }
  auto& heap = execution.isolate().heap();
  auto* object = heap.AllocateInstance(layout, captures);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  heap.SetNativeEnvironment(*object, *selected.state, selected.function);
  if (target.mode == CallableMode::InterfaceMethod && !captures.empty()) {
    const auto captured = CaptureNativeReceiver(execution, *object);
    if (!captured) {
      return std::unexpected(captured.error());
    }
  }
  return object;
}

std::expected<void, ExecutionStatus> CaptureNativeReceiver(NativeExecution& execution,
                                                           HeapObject& callable) {
  const auto target = *Heap::ClassOf(callable)->callable();
  auto& source = Heap::NativeProgram(*Heap::NativeEnvironment(callable));
  if (source.dispatch == nullptr || target.mode != CallableMode::InterfaceMethod) {
    return {};
  }
  const auto receiver = *Heap::GetField(callable, 0);
  const auto* layout = receiver.IsObject() ? Heap::ClassOf(*receiver.AsObject()) : nullptr;
  if (layout == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto selector =
      source.image->selector_declarations.values().subspan(target.function).front();
  const auto selected = source.dispatch->ResolveInterface(layout->nominal_type(), selector);
  if (!selected) {
    return std::unexpected(selected.error());
  }
  execution.isolate().heap().SetNativeEnvironment(callable, *selected->state, selected->function);
  return {};
}

extern "C" std::uint8_t vela_native_v3_capture_receiver(NativeExecution* execution,
                                                        HeapObject* callable) {
  const auto captured = CaptureNativeReceiver(*execution, *callable);
  return static_cast<std::uint8_t>(captured ? ExecutionStatus::Returned : captured.error());
}

std::expected<NativeResolvedCallable, ExecutionStatus> ResolveNativeCallable(
    const NativeInstance& caller,
    Value callable) {
  const auto resolved = ResolveNativeCallback(Domain(caller), callable);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  return NativeResolvedCallable{.state = resolved->environment, .target = resolved->target};
}

std::expected<Value, ExecutionStatus> InvokeNativeEntry(NativeExecution& execution,
                                                        std::uint32_t function,
                                                        std::span<const Value> input) {
  auto& instance = execution.instance();
  const auto functions = instance.image().functions.values();
  if (function >= functions.size()) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  const auto& target = functions.subspan(function).front();
  const auto& parameters = instance.call_signature(function).parameters;
  if ((target.flags & kNativeExportedFunction) == 0 || target.hidden_parameters != 0 ||
      parameters.size() != input.size()) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  const RootRange input_root(execution.isolate().heap(), input.data(), input.size(),
                             RootOwner::Runtime);
  std::vector<NativeValue> arguments;
  arguments.reserve(input.size());
  const auto& types = instance.layouts().types;
  for (std::size_t index = 0; index < input.size(); ++index) {
    auto value = input.subspan(index).front();
    const auto& type = types.at(parameters.at(index).type);
    if (!Heap::IsValueOfType(value, type)) {
      return std::unexpected(ExecutionStatus::InvalidCast);
    }
    if (Heap::TypeOf(value).callable) {
      if (const auto callable = ResolveNativeCallable(instance, value); !callable) {
        return std::unexpected(callable.error());
      }
    }
    if (!value.IsNull() && type.nominal != nullptr && type.nominal->IsEnum()) {
      value = *Heap::GetField(*value.AsObject(), 0);
    }
    arguments.push_back(EncodeNativeValue(value));
  }
  return InvokeNativeTarget(execution, function, arguments);
}

std::expected<Value, ExecutionStatus> internal::BoxNativeEnumResult(NativeExecution& execution,
                                                                    std::uint32_t type,
                                                                    Value value) {
  const auto& instance = execution.instance();
  return BoxDynamicValue(execution.isolate().heap(), value, instance.layouts().types.at(type));
}

FutureCallbacks NativeFutureCallbacks(const NativeInstance& instance) {
  const auto domain = Domain(instance);
  return {
      .arity = [domain](Value callable) -> std::expected<std::size_t, ExecutionStatus> {
        const auto callback = ResolveNativeCallback(domain, callable);
        if (!callback) {
          return std::unexpected(callback.error());
        }
        return callback->function->parameter_count - (callback->target.implicit ? 1U : 0U);
      },
      .invoke =
          [domain](Value callable, std::span<const Value> arguments) {
            return InvokeFutureCallback(domain, callable, arguments);
          },
  };
}

extern "C" std::uint8_t vela_native_v1_call_indirect(NativeExecution* execution,
                                                     const NativeValue* callable,
                                                     NativeValue* arguments,
                                                     std::uint64_t count,
                                                     NativeValue* result) {
  const auto& instance = execution->instance();
  const auto value = DecodeNativeValue(*callable);
  const auto resolved = ResolveNativeCallable(instance, value);
  if (!resolved) {
    return static_cast<std::uint8_t>(resolved.error());
  }
  auto origin = NativeInstance::Borrow(*resolved->state);
  const NativeInstanceScope scope(*execution, origin);
  const auto input = std::span(arguments, count + 1);
  const auto& target = resolved->target;
  const auto hidden = target.implicit ? 1U : 0U;
  if (target.implicit) {
    input.front() = EncodeNativeValue(*target.implicit);
  }
  return origin.image()
      .functions.values()
      .subspan(target.function)
      .front()
      .entry(execution, input.subspan(1 - hidden).data(), count + hidden, result);
}

extern "C" std::uint8_t vela_native_v1_call_interface(NativeExecution* execution,
                                                      std::uint32_t selector,
                                                      const NativeValue* arguments,
                                                      std::uint64_t count,
                                                      NativeValue* result) {
  const auto functions = execution->instance().image().functions.values();
  const auto target =
      ResolveInterfaceTarget(selector, DecodeNativeValue(*arguments), functions.size());
  if (!target) {
    return static_cast<std::uint8_t>(target.error());
  }
  return functions.subspan(*target).front().entry(execution, arguments, count, result);
}

extern "C" std::uint8_t vela_native_v3_call_declaration(NativeExecution* execution,
                                                        std::uint64_t declaration,
                                                        const NativeValue* arguments,
                                                        std::uint64_t count,
                                                        NativeValue* result) {
  const auto& instance = execution->instance();
  auto& dispatch = *Heap::NativeProgram(*instance.state().front().AsObject()).dispatch;
  const auto target = dispatch.Resolve(DeclarationId{declaration});
  if (!target) {
    const auto status = target.error();
    return static_cast<std::uint8_t>(
        status == ExecutionStatus::TargetDeleted
            ? execution->HandleFailure(status, "declaration " + std::to_string(declaration) +
                                                   " was deleted in revision " +
                                                   std::to_string(dispatch.revision()))
            : status);
  }
  auto origin = NativeInstance::Borrow(*target->state);
  const NativeInstanceScope scope(*execution, origin);
  return origin.image()
      .functions.values()
      .subspan(target->function)
      .front()
      .entry(execution, arguments, count, result);
}

extern "C" std::uint8_t vela_native_v3_call_interface(NativeExecution* execution,
                                                      std::uint64_t selector,
                                                      const NativeValue* arguments,
                                                      std::uint64_t count,
                                                      NativeValue* result) {
  const auto receiver = DecodeNativeValue(*arguments);
  const auto* layout = receiver.IsObject() ? Heap::ClassOf(*receiver.AsObject()) : nullptr;
  if (layout == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::TypeError);
  }
  auto& dispatch = *Heap::NativeProgram(*execution->instance().state().front().AsObject()).dispatch;
  const auto target = dispatch.ResolveInterface(layout->nominal_type(), DeclarationId{selector});
  if (!target) {
    const auto status = target.error();
    return static_cast<std::uint8_t>(
        status == ExecutionStatus::TargetDeleted
            ? execution->HandleFailure(status, "interface selector " + std::to_string(selector) +
                                                   " was deleted in revision " +
                                                   std::to_string(dispatch.revision()))
            : status);
  }
  auto origin = NativeInstance::Borrow(*target->state);
  const NativeInstanceScope scope(*execution, origin);
  return origin.image()
      .functions.values()
      .subspan(target->function)
      .front()
      .entry(execution, arguments, count, result);
}

}  // namespace vela::runtime
