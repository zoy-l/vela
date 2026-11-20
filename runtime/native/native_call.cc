#include "runtime/native/native_call.h"
#include <optional>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "base/callable.h"
#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/async/future_composition.h"
#include "runtime/async/future_operations.h"
#include "runtime/dynamic/dynamic_call.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/native_buffer.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_program_state.h"
#include "runtime/native/native_stack.h"
#include "runtime/value/callable.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {
namespace {

// 同步实参覆盖整次调用，严格按栈序退出；与生成帧共用根链，并保留新根发布屏障。
class NativeArgumentRoots final {
 public:
  NativeArgumentRoots(Heap& heap, std::span<const Value> values) noexcept
      : head_(heap.native_stack_roots_head()),
        frame_{.values = values.data(), .count = values.size(), .previous = *head_} {
    *head_ = &frame_;
    heap.WriteRootBarrier(values);
  }
  ~NativeArgumentRoots() { *head_ = frame_.previous; }
  NativeArgumentRoots(const NativeArgumentRoots&) = delete;
  NativeArgumentRoots& operator=(const NativeArgumentRoots&) = delete;
  NativeArgumentRoots(NativeArgumentRoots&&) = delete;
  NativeArgumentRoots& operator=(NativeArgumentRoots&&) = delete;

 private:
  NativeStackRoots** head_;
  NativeStackRoots frame_;
};

template <class Arguments>
std::expected<Value, ExecutionStatus> InvokeBoundTarget(NativeExecution& execution,
                                                        std::uint32_t function,
                                                        const CallSignature& signature,
                                                        const BoundArguments& bound,
                                                        const Arguments& input,
                                                        std::optional<Value> receiver,
                                                        NativeArgumentPresence presence) {
  auto& heap = execution.isolate().heap();
  const auto count = bound.source_indices.size();
  const auto hidden = receiver ? 1U : 0U;
  const auto append_presence =
      presence == NativeArgumentPresence::Included || signature.HasDefaults();
  constexpr std::size_t kInlineArguments = 8;
  NativeBuffer<Value, kInlineArguments> storage(heap,
                                                hidden + count + (append_presence ? count : 0));
  if (!storage.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto values = storage.values();
  std::uninitialized_value_construct_n(values.data(), values.size());
  const RootRange roots(heap, values.data(), values.size(), std::nullopt);
  if (receiver) {
    values.front() = *receiver;
  }
  const auto& types = execution.instance().layouts().types;
  for (std::size_t index = 0; index < count; ++index) {
    const auto value = ReadDynamicArgument(heap, signature, types, input, bound, index);
    if (!value) {
      return std::unexpected(value.error());
    }
    values[hidden + index] = *value;
    if (append_presence) {
      values[hidden + count + index] =
          Value::Boolean(bound.source_indices[index] != BoundArguments::kUnassigned);
    }
  }
  NativeBuffer<NativeValue, kInlineArguments> encoded(heap, values.size());
  if (!encoded.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  std::ranges::transform(values, encoded.values().begin(), EncodeNativeValue);
  return InvokeNativeTarget(execution, function, encoded.values());
}

struct NativeCallDomain final {
  Isolate* isolate;
  const CodeIdentity* identity;
  const NativeDispatch* dispatch;
};

NativeCallDomain Domain(const NativeInstance& instance) {
  return {
      .isolate = &instance.isolate(),
      .identity = &instance.code_identity(),
      .dispatch = instance.dispatch(),
  };
}

std::expected<std::uint32_t, ExecutionStatus>
ResolveClosedVirtualTarget(const NativeInstance& instance, std::uint32_t selector, Value receiver) {
  const auto* layout = receiver.IsObject() ? Heap::ClassOf(*receiver.AsObject()) : nullptr;
  if (layout == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  // DateParts 等固定布局跨程序共享；函数编号只属于当前映像，不能存入对象布局。
  const auto& state = instance.program();
  const auto found = state.class_indices.find(&layout->nominal_type());
  if (found == state.class_indices.end()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto targets = instance.image().classes.values()[found->second].dispatch_targets.values();
  if (selector >= targets.size() || targets[selector] == kInvalidFunctionTarget) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return targets[selector];
}

std::uint8_t InvokeResolvedDeclaration(NativeExecution* execution,
                                       const NativeResolvedDeclaration& target,
                                       const NativeValue* arguments,
                                       std::uint64_t count,
                                       NativeValue* result) {
  auto origin = NativeInstance::Borrow(*target.state);
  const NativeInstanceScope scope(*execution, origin);
  return origin.image().function(target.function).entry(execution, arguments, count, result);
}

std::expected<NativeResolvedCallable, ExecutionStatus> ResolveCallableInDomain(
    const NativeCallDomain& domain,
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
  ResolvedCallable target{.function = Heap::NativeFunctionIndex(*object)};
  switch (Heap::ClassOf(*object)->callable()->mode) {
    case CallableMode::Plain:
      break;
    case CallableMode::Closure:
      target.implicit = callable;
      break;
    case CallableMode::BoundMethod:
    case CallableMode::VirtualMethod:
      target.implicit = Heap::LoadField(*object, 0);
      break;
  }
  return NativeResolvedCallable{
      .state = environment,
      .target = target,
      .function = &functions.subspan(target.function).front(),
  };
}

Future FailedCallback(Isolate& isolate,
                      ExecutionStatus status,
                      Value payload = Value::Null(),
                      FutureDiagnostic diagnostic = {}) {
  Future result(&isolate.scheduler());
  result.AttachHeap(isolate.heap());
  diagnostic.status = status;
  CompleteFutureError(isolate, result, payload, diagnostic);
  return result;
}

bool HasArrayLayout(Value value, const ArrayLayout& layout) {
  const auto* object = value.IsObject() ? value.AsObject() : nullptr;
  return object != nullptr && Heap::ArrayTypeOf(*object) == &layout;
}

std::expected<Value, ExecutionStatus> ConvertCallbackArgument(const NativeInstance& instance,
                                                              const NativeCallParameter& parameter,
                                                              Value value) {
  const auto& declared = instance.layouts().types.at(parameter.type);
  const auto optional = parameter.IsOptional() && !parameter.HasDefault();
  if (const auto* array = declared.array; array != nullptr) {
    // 规范数组布局完整表达容器约束；无需 nominal、enum 或 callable 转换。
    if (value.IsNull() && (declared.nullable || optional)) {
      return value;
    }
    return HasArrayLayout(value, *array) ? std::expected<Value, ExecutionStatus>(value)
                                         : std::unexpected(ExecutionStatus::InvalidCast);
  }
  auto type = declared;
  if (optional) {
    type.nullable = true;
  }
  if (!Heap::IsValueOfType(value, type)) {
    return std::unexpected(ExecutionStatus::InvalidCast);
  }
  const auto* layout = value.IsObject() ? Heap::ClassOf(*value.AsObject()) : nullptr;
  if (layout != nullptr && layout->callable() != nullptr) {
    if (const auto checked = ResolveNativeCallable(instance, value); !checked) {
      return std::unexpected(checked.error());
    }
  }
  return UnboxDynamicValue(value, type);
}

void EncodePresence(std::span<NativeValue> output, std::size_t supplied) {
  for (std::size_t index = 0; index < output.size(); ++index) {
    output[index] = EncodeNativeValue(Value::Boolean(index < supplied));
  }
}

std::expected<Value, ExecutionStatus> InvokeCallback(NativeExecution& execution,
                                                     const NativeResolvedCallable& callback,
                                                     std::span<const Value> arguments) {
  const auto& function = *callback.function;
  const auto& target = callback.target;
  const auto hidden = target.implicit ? 1U : 0U;
  const auto& instance = execution.instance();
  const auto& types = instance.layouts().types;
  const auto typed = (function.flags & kNativeTypedFunction) != 0;
  const auto parameters = function.parameters.values();
  const auto has_defaults = function.HasDefaults();
  const auto logical_count = function.parameter_count - (has_defaults ? parameters.size() : 0);
  if (arguments.size() + hidden > logical_count) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (!typed && function.parameter_count != arguments.size() + hidden) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (function.parameter_count == 0) {
    // 数量检查已排除实参和隐式接收者；没有需要编码、补缺省或保根的输入。
    return InvokeNativeTarget(execution, function, {});
  }
  auto& heap = execution.isolate().heap();
  const NativeArgumentRoots arguments_root(heap, arguments);
  constexpr std::size_t kInlineArguments = 8;
  // 显式实参已保根；没有隐式实参或缺省值时不构造额外存储和空根区间。
  const auto owned_count = function.parameter_count - arguments.size();
  std::optional<NativeBuffer<Value, kInlineArguments>> storage;
  std::span<Value> values;
  RootRange roots(std::nullopt);
  if (owned_count != 0) {
    storage.emplace(heap, owned_count);
    if (!storage->IsAllocated()) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    values = storage->values();
    std::uninitialized_value_construct_n(values.data(), values.size());
    roots.Attach(heap, values.data(), values.size());
  }
  if (target.implicit) {
    values.front() = *target.implicit;
  }
  NativeBuffer<NativeValue, kInlineArguments> input(heap, function.parameter_count);
  if (!input.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto encoded = input.values();
  if (target.implicit) {
    encoded.front() = EncodeNativeValue(*target.implicit);
  }
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    auto value = arguments[index];
    if (typed) {
      const auto parameter = parameters[index + hidden - function.hidden_parameters];
      const auto converted = ConvertCallbackArgument(instance, parameter, value);
      if (!converted) {
        return std::unexpected(converted.error());
      }
      value = *converted;
    }
    encoded[index + hidden] = EncodeNativeValue(value);
  }
  for (std::size_t index = arguments.size() + hidden; index < logical_count; ++index) {
    const auto parameter = parameters[index - function.hidden_parameters];
    auto& value = values[index - arguments.size()];
    if ((parameter.flags & kNativeRestParameter) != 0) {
      auto* empty = heap.AllocateArray(*types.at(parameter.type).array, 0);
      if (empty == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      value = Value::Object(empty);
    } else if ((parameter.flags & kNativeOptionalParameter) == 0) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    encoded[index] = EncodeNativeValue(value);
  }
  EncodePresence(encoded.subspan(logical_count), arguments.size());
  return InvokeNativeTarget(execution, function, encoded);
}

Future InvokeFutureCallback(const NativeCallDomain& domain,
                            const NativeResolvedCallable& callback,
                            Value callable,
                            std::span<const Value> arguments) {
  auto& isolate = *domain.isolate;
  const RootRange callable_root(isolate.heap(), &callable, 1, std::nullopt);
  auto instance = NativeInstance::Borrow(*callback.state);
  NativeExecution execution(instance);
  const auto invoked = InvokeCallback(execution, callback, arguments);
  if (invoked) {
    return RootFutureValue(isolate, *invoked);
  }
  const auto status = execution.HandleFailure(invoked.error());
  return FailedCallback(isolate, status, execution.ExceptionPayload(),
                        execution.FailureDiagnostic(status));
}

std::expected<void, ExecutionStatus> CaptureNativeReceiver(NativeExecution& execution,
                                                           HeapObject& callable) {
  const auto target = *Heap::ClassOf(callable)->callable();
  if (target.mode != CallableMode::VirtualMethod) {
    return {};
  }
  const auto receiver = Heap::LoadField(callable, 0);
  const auto origin = NativeInstance::Borrow(*Heap::NativeEnvironment(callable));
  const auto selected = ResolveNativeMethod(origin, target, receiver);
  if (!selected) {
    return std::unexpected(selected.error());
  }
  execution.isolate().heap().SetNativeEnvironment(callable, *selected->state,
                                                  selected->target.function);
  return {};
}

}  // namespace

std::expected<Value, ExecutionStatus> InvokeBoundNativeTarget(NativeExecution& execution,
                                                              std::uint32_t function,
                                                              const CallSignature& signature,
                                                              const BoundArguments& bound,
                                                              const HeapObject& arguments,
                                                              std::optional<Value> receiver) {
  return InvokeBoundTarget(execution, function, signature, bound, arguments, receiver,
                           NativeArgumentPresence::Absent);
}

std::expected<Value, ExecutionStatus> InvokeBoundNativeTarget(NativeExecution& execution,
                                                              std::uint32_t function,
                                                              const CallSignature& signature,
                                                              const BoundArguments& bound,
                                                              std::span<const Value> arguments,
                                                              std::optional<Value> receiver,
                                                              NativeArgumentPresence presence) {
  return InvokeBoundTarget(execution, function, signature, bound, arguments, receiver, presence);
}

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
      target.mode != CallableMode::VirtualMethod) {
    const auto identity = state.image->function(target.function).identity;
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
  if (target.mode == CallableMode::VirtualMethod && !captures.empty()) {
    const auto captured = CaptureNativeReceiver(execution, *object);
    if (!captured) {
      return std::unexpected(captured.error());
    }
  }
  return object;
}

extern "C" std::uint8_t vela_native_v3_capture_receiver(NativeExecution* execution,
                                                        HeapObject* callable) {
  const auto captured = CaptureNativeReceiver(*execution, *callable);
  return ToNativeStatus(captured);
}

std::expected<NativeResolvedCallable, ExecutionStatus>
ResolveNativeMethod(const NativeInstance& caller, CallableTarget method, Value receiver) {
  NativeResolvedDeclaration selected{
      .state = caller.state().front().AsObject(),
      .function = method.function,
  };
  if (method.mode == CallableMode::VirtualMethod) {
    if (const auto* dispatch = caller.dispatch()) {
      const auto* layout = receiver.IsObject() ? Heap::ClassOf(*receiver.AsObject()) : nullptr;
      if (layout == nullptr) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      const auto selector = caller.image().selector_declarations.values()[method.function];
      const auto target = dispatch->ResolveVirtual(layout->nominal_type(), selector);
      if (!target) {
        return std::unexpected(target.error());
      }
      selected = *target;
    } else {
      const auto target = ResolveClosedVirtualTarget(caller, method.function, receiver);
      if (!target) {
        return std::unexpected(target.error());
      }
      selected.function = *target;
    }
  }
  return NativeResolvedCallable{
      .state = selected.state,
      .target = {.function = selected.function, .implicit = receiver},
      .function = &Heap::NativeProgram(*selected.state).image->function(selected.function),
  };
}

std::expected<NativeResolvedCallable, ExecutionStatus> ResolveNativeCallable(
    const NativeInstance& caller,
    Value callable) {
  return ResolveCallableInDomain(Domain(caller), callable);
}

std::expected<NativeCallableContract, ExecutionStatus> InspectNativeCallable(
    const NativeInstance& caller,
    Value callable) {
  const auto resolved = ResolveNativeCallable(caller, callable);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  const auto& state = Heap::NativeProgram(*resolved->state);
  const auto index = resolved->target.function;
  const auto& function = state.image->function(index);
  if ((function.flags & kNativeTypedFunction) == 0) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& signature = state.signatures.at(index);
  const auto skipped = (resolved->target.implicit ? 1U : 0U) - signature.hidden_parameters;
  return NativeCallableContract{
      .signature = &signature,
      .parameters = std::span(signature.parameters).subspan(skipped),
      .types = state.layouts.types,
      .is_async = (function.flags & kNativeAsyncFunction) != 0,
  };
}

[[clang::always_inline]] std::expected<Value, ExecutionStatus> InvokeNativeEntry(
    NativeExecution& execution,
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
      parameters.size() < input.size()) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  return InvokeCallback(execution, {.target = {.function = function}, .function = &target}, input);
}

namespace {

ExecutionStatus MaterializeHostContext(NativeExecution& execution,
                                       NativeHostContext& context,
                                       std::uint8_t status) {
  if (status == kNativeHostEnter && context.depth >= kMaxCallFrames) {
    return ExecutionStatus::StackOverflow;
  }
  if (context.trace_count == 0) {
    return static_cast<ExecutionStatus>(status);
  }
  std::array<NativeTraceFrame, kNativeHostTraceCapacity> frames;
  for (std::uint32_t index = 0; index < context.trace_count; ++index) {
    const auto position = DecodeNativeTracePosition(context.trace[context.trace_count - index - 1]);
    auto& frame = frames[index];
    execution.MaterializeFrame(frame, position.function);
    frame.position.instruction = position.instruction;
  }
  const auto observe = status == kNativeHostObserve || status == kNativeHostEnter;
  auto result = observe ? static_cast<ExecutionStatus>(vela_native_v3_safepoint(&execution))
                        : static_cast<ExecutionStatus>(status);
  if (!observe || result != ExecutionStatus::Returned) {
    result = execution.HandleFailure(result);
  }
  for (std::uint32_t index = 0; index < context.trace_count; ++index) {
    execution.Exit();
  }
  return result;
}

std::expected<Value, ExecutionStatus> CompleteInvocation(
    NativeExecution& execution,
    std::expected<Value, ExecutionStatus> result,
    const NativeFailureObserver* failure) {
  if (!result && failure != nullptr) {
    failure->capture(failure->context, execution, result.error());
  }
  return result;
}

[[clang::noinline]] std::expected<Value, ExecutionStatus> RejectArrayEntry(
    NativeInstance& instance,
    std::span<const Value> input,
    const NativeExecution* parent,
    const NativeFailureObserver* failure,
    ExecutionStatus status) {
  if (failure == nullptr) {
    return std::unexpected(status);
  }
  const NativeArgumentRoots roots(instance.isolate().heap(), input);
  NativeExecution execution(instance, parent);
  return CompleteInvocation(execution, std::unexpected(status), failure);
}

// 真正执行资源只存在于此冷路径；观察完成后的输出直接返回，不能重放已完成的字段写入。
[[clang::noinline]] std::expected<Value, ExecutionStatus> ExecuteArrayEntry(
    NativeInstance& instance,
    const NativeFunctionDescription& function,
    std::span<const Value> input,
    const NativeExecution* parent,
    const NativeFailureObserver* failure,
    NativeHostContext* context,
    std::uint8_t status,
    const NativeValue* output) {
  const NativeArgumentRoots roots(instance.isolate().heap(), input);
  NativeExecution execution(instance, parent);
  if (context != nullptr) {
    const auto completed = MaterializeHostContext(execution, *context, status);
    if (completed != ExecutionStatus::Returned) {
      return CompleteInvocation(execution, std::unexpected(completed), failure);
    }
    if (status != kNativeHostEnter) {
      return DecodeNativeValue(*output);
    }
  }
  const std::array arguments{EncodeNativeValue(input.front())};
  return CompleteInvocation(execution, InvokeNativeTarget(execution, function, arguments), failure);
}

}  // namespace

[[clang::noinline]] std::expected<Value, ExecutionStatus> InvokeNativeEntry(
    NativeInstance& instance,
    std::uint32_t function,
    std::span<const Value> input,
    const NativeExecution* parent,
    const NativeFailureObserver* failure) {
  NativeExecution execution(instance, parent);
  return CompleteInvocation(execution, InvokeNativeEntry(execution, function, input), failure);
}

[[clang::always_inline]] std::expected<Value, ExecutionStatus> InvokeNativeArrayEntry(
    NativeInstance& instance,
    const NativeFunctionDescription& function,
    const ArrayLayout& array,
    std::span<const Value> input,
    NativeHostEntry selected_host,
    const NativeExecution* parent,
    const NativeFailureObserver* failure) {
  if (input.size() != 1) {
    return RejectArrayEntry(instance, input, parent, failure, ExecutionStatus::InvalidEntry);
  }
  auto& isolate = instance.isolate();
  auto& heap = isolate.heap();
  const auto host = !instance.image().IsReloadable() && !heap.IsCollecting() &&
                            (function.flags & kNativeEnumResult) == 0
                        ? selected_host
                        : nullptr;
  const auto value = input.front();
  if (!HasArrayLayout(value, array)) {
    return RejectArrayEntry(instance, input, parent, failure, ExecutionStatus::InvalidCast);
  }
  auto* object = value.AsObject();
  if (host != nullptr) {
    const auto limit = CurrentNativeStackLimit();
    NativeHostContext context(instance.program(), isolate.native_control().state(),
                              parent == nullptr ? 0 : parent->frame_count(), limit);
    NativeValue output;
    const auto status = host(&context, object, &output);
    if (status == ToNativeStatus(ExecutionStatus::Returned)) {
      return DecodeNativeValue(output);
    }
    if (status != kNativeHostMiss) {
      return ExecuteArrayEntry(instance, function, input, parent, failure, &context, status,
                               &output);
    }
  }
  return ExecuteArrayEntry(instance, function, input, parent, failure, nullptr, kNativeHostMiss,
                           nullptr);
}

[[clang::always_inline]] std::expected<Value, ExecutionStatus> InvokeNativeFunction(
    NativeExecution& execution,
    std::uint32_t function,
    std::span<const Value> arguments) {
  const auto functions = execution.instance().image().functions.values();
  if (function >= functions.size()) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  const auto& description = functions.subspan(function).front();
  if (description.hidden_parameters != 0) {
    return std::unexpected(ExecutionStatus::InvalidEntry);
  }
  return InvokeCallback(execution, {.target = {.function = function}, .function = &description},
                        arguments);
}

std::expected<Value, ExecutionStatus> InvokeNativeCallable(NativeExecution& execution,
                                                           Value callable,
                                                           std::span<const Value> arguments) {
  auto& heap = execution.isolate().heap();
  const RootRange callable_root(heap, &callable, 1, std::nullopt);
  const auto callback = ResolveCallableInDomain(Domain(execution.instance()), callable);
  if (!callback) {
    return std::unexpected(callback.error());
  }
  auto origin = NativeInstance::Borrow(*callback->state);
  const NativeInstanceScope scope(execution, origin);
  return InvokeCallback(execution, *callback, arguments);
}

std::expected<Value, ExecutionStatus> internal::BoxNativeEnumResult(const NativeInstance& instance,
                                                                    std::uint32_t type,
                                                                    Value value) {
  return BoxDynamicValue(instance.isolate().heap(), value, instance.layouts().types[type]);
}

bool NativeCallableContract::HasRequiredParameters(std::size_t skipped) const {
  return std::ranges::any_of(parameters.subspan(skipped), [](const auto& parameter) {
    return !parameter.optional && !parameter.rest;
  });
}

FutureCallbacks NativeFutureCallbacks(const NativeInstance& instance) {
  const auto domain = Domain(instance);
  return {
      .resolve = [domain](Value callable) -> std::expected<FutureCallback, ExecutionStatus> {
        const auto callback = ResolveCallableInDomain(domain, callable);
        if (!callback) {
          return std::unexpected(callback.error());
        }
        return FutureCallback{
            .arity = static_cast<std::uint32_t>(
                Heap::ClassOf(*callable.AsObject())->callable_type()->parameters.size()),
            .invoke =
                [domain, resolved = *callback, callable](std::span<const Value> arguments) {
                  return InvokeFutureCallback(domain, resolved, callable, arguments);
                },
        };
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
  const auto& function = origin.image().function(target.function);
  const auto supplied = input.subspan(1 - hidden);
  if (!function.HasDefaults()) {
    return function.entry(execution, supplied.data(), supplied.size(), result);
  }
  constexpr std::size_t kInlineArguments = 8;
  NativeBuffer<NativeValue, kInlineArguments> completed(execution->isolate().heap(),
                                                        function.parameter_count);
  if (!completed.IsAllocated()) {
    return static_cast<std::uint8_t>(ExecutionStatus::OutOfMemory);
  }
  const auto values = completed.values();
  std::ranges::copy(supplied, values.begin());
  const auto logical =
      static_cast<std::size_t>(function.parameter_count - function.parameters.size);
  std::ranges::fill(values.subspan(supplied.size(), logical - supplied.size()),
                    EncodeNativeValue(Value::Null()));
  EncodePresence(values.subspan(logical), count);
  return function.entry(execution, values.data(), values.size(), result);
}

extern "C" std::uint8_t vela_native_v1_call_virtual(NativeExecution* execution,
                                                    std::uint32_t selector,
                                                    const NativeValue* arguments,
                                                    std::uint64_t count,
                                                    NativeValue* result) {
  const auto& instance = execution->instance();
  const auto target = ResolveClosedVirtualTarget(instance, selector, DecodeNativeValue(*arguments));
  if (!target) {
    return static_cast<std::uint8_t>(target.error());
  }
  return instance.image().function(*target).entry(execution, arguments, count, result);
}

extern "C" std::uint8_t vela_native_v3_call_declaration(NativeExecution* execution,
                                                        std::uint64_t declaration,
                                                        const NativeValue* arguments,
                                                        std::uint64_t count,
                                                        NativeValue* result) {
  const auto& instance = execution->instance();
  auto& dispatch = *instance.dispatch();
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
  return InvokeResolvedDeclaration(execution, *target, arguments, count, result);
}

extern "C" std::uint8_t vela_native_v3_call_virtual(NativeExecution* execution,
                                                    std::uint64_t selector,
                                                    const NativeValue* arguments,
                                                    std::uint64_t count,
                                                    NativeValue* result) {
  const auto receiver = DecodeNativeValue(*arguments);
  const auto* layout = receiver.IsObject() ? Heap::ClassOf(*receiver.AsObject()) : nullptr;
  if (layout == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::TypeError);
  }
  auto& dispatch = *execution->instance().dispatch();
  const auto target = dispatch.ResolveVirtual(layout->nominal_type(), DeclarationId{selector});
  if (!target) {
    const auto status = target.error();
    return static_cast<std::uint8_t>(
        status == ExecutionStatus::TargetDeleted
            ? execution->HandleFailure(status, "virtual selector " + std::to_string(selector) +
                                                   " was deleted in revision " +
                                                   std::to_string(dispatch.revision()))
            : status);
  }
  return InvokeResolvedDeclaration(execution, *target, arguments, count, result);
}

}  // namespace vela::runtime
