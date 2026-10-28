#include "runtime/native_call.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "base/execution_status.h"
#include "runtime/callable.h"
#include "runtime/exception.h"
#include "runtime/future.h"
#include "runtime/future_composition.h"
#include "runtime/future_operations.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
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

std::expected<NativeCallback, ExecutionStatus> ResolveNativeCallback(Isolate& isolate,
                                                                     const CodeIdentity& identity,
                                                                     Value callable) {
  auto* object = callable.IsObject() ? callable.AsObject() : nullptr;
  auto* environment = object != nullptr ? Heap::NativeEnvironment(*object) : nullptr;
  if (environment == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& state = Heap::NativeProgram(*environment);
  if (state.isolate != &isolate) {
    return std::unexpected(ExecutionStatus::InvalidIsolate);
  }
  const auto functions = state.image->functions.values();
  const auto target = ResolveCallable(callable, identity, functions.size());
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
      if (!value.IsNull() && type.instance != nullptr && type.instance->IsEnum()) {
        value = *Heap::GetField(*value.AsObject(), 0);
      }
    }
    input.at(index + hidden) = EncodeNativeValue(value);
  }
  return InvokeNativeTarget(execution, target.function, input);
}

Future InvokeFutureCallback(Isolate& isolate,
                            const CodeIdentity& identity,
                            Value callable,
                            std::span<const Value> arguments) {
  const RootRange callable_root(isolate.heap(), &callable, 1);
  const RootRange arguments_root(isolate.heap(), arguments.data(), arguments.size());
  const auto callback = ResolveNativeCallback(isolate, identity, callable);
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

std::expected<Value, ExecutionStatus> InvokeNativeTarget(NativeExecution& execution,
                                                         std::uint32_t function,
                                                         std::span<const NativeValue> arguments) {
  const auto& instance = execution.instance();
  const auto& target = instance.image().functions.values().subspan(function).front();
  NativeValue output;
  const auto status = static_cast<ExecutionStatus>(
      target.entry(&execution, arguments.data(), arguments.size(), &output));
  if (status != ExecutionStatus::Returned) {
    return std::unexpected(status);
  }
  const auto value = DecodeNativeValue(output);
  // 只有 enum 的标量载荷需要分配盒装对象；引用结果原样返回，期间没有 GC 安全点。
  return target.result_type != kNoNativeIndex
             ? BoxDynamicValue(execution.isolate().heap(), value,
                               instance.layouts().types.at(target.result_type))
             : std::expected<Value, ExecutionStatus>(value);
}

FutureCallbacks NativeFutureCallbacks(const NativeInstance& instance) {
  auto* isolate = &instance.isolate();
  const auto* identity = &instance.code_identity();
  return {
      .arity = [isolate, identity](Value callable) -> std::expected<std::size_t, ExecutionStatus> {
        const auto callback = ResolveNativeCallback(*isolate, *identity, callable);
        if (!callback) {
          return std::unexpected(callback.error());
        }
        return callback->function->parameter_count - (callback->target.implicit ? 1U : 0U);
      },
      .invoke =
          [isolate, identity](Value callable, std::span<const Value> arguments) {
            return InvokeFutureCallback(*isolate, *identity, callable, arguments);
          },
  };
}

extern "C" std::uint8_t vela_native_v1_call_indirect(NativeExecution* execution,
                                                     const NativeValue* callable,
                                                     NativeValue* arguments,
                                                     std::uint64_t count,
                                                     NativeValue* result) {
  const auto& instance = execution->instance();
  const auto functions = instance.image().functions.values();
  const auto value = DecodeNativeValue(*callable);
  const auto resolved = ResolveCallable(value, instance.code_identity(), functions.size());
  if (!resolved) {
    return static_cast<std::uint8_t>(resolved.error());
  }
  auto* environment = Heap::NativeEnvironment(*value.AsObject());
  if (environment == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::TypeError);
  }
  auto origin = NativeInstance::Borrow(*environment);
  const NativeInstanceScope scope(*execution, origin);
  const auto input = std::span(arguments, count + 1);
  const auto hidden = resolved->implicit ? 1U : 0U;
  if (resolved->implicit) {
    input.front() = EncodeNativeValue(*resolved->implicit);
  }
  return functions.subspan(resolved->function)
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

}  // namespace vela::runtime
