#include "runtime/native_dynamic.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "base/builtin.h"
#include "base/callable.h"
#include "base/execution_status.h"
#include "base/member.h"
#include "runtime/callable.h"
#include "runtime/dynamic_array.h"
#include "runtime/dynamic_call.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_call.h"
#include "runtime/native_image.h"
#include "runtime/native_instance.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "runtime/value_buffer.h"
#include "runtime/value_operations.h"

namespace vela::runtime {
namespace {

std::expected<ResolvedCallable, ExecutionStatus> Resolve(NativeExecution& execution, Value value) {
  const auto& instance = execution.instance();
  return ResolveCallable(value, instance.code_identity(), instance.image().functions.size);
}

std::expected<Value, ExecutionStatus> CreatePlan(
    NativeExecution& execution,
    Value value,
    std::optional<std::uint32_t> target = std::nullopt) {
  if (!target) {
    if (const auto resolved = Resolve(execution, value); !resolved) {
      return std::unexpected(resolved.error());
    }
  }
  return CreateDynamicCallPlan(execution.isolate().heap(), {.value = value, .method = target});
}

std::expected<Value, ExecutionStatus> InvokeDynamic(NativeExecution& execution,
                                                    const ResolvedCallable& callable,
                                                    const HeapObject& arguments,
                                                    const HeapObject& names,
                                                    DynamicCallStyle style) {
  const auto& instance = execution.instance();
  const auto& signature = instance.call_signature(callable.function);
  const auto types = std::span(instance.layouts().types);
  const auto bound = BindDynamicArguments(signature, callable, arguments, names, style);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  if (const auto checked = CheckDynamicArguments(signature, types, arguments, *bound); !checked) {
    return std::unexpected(checked.error());
  }
  const auto hidden = callable.implicit ? 1U : 0U;
  ValueBuffer storage(bound->source_indices.size() + hidden);
  if (!storage.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto values = storage.values();
  const RootRange roots(execution.isolate().heap(), values.data(), values.size());
  if (callable.implicit) {
    values.front() = *callable.implicit;
  }
  for (std::size_t index = 0; index < bound->source_indices.size(); ++index) {
    const auto value =
        ReadDynamicArgument(execution.isolate().heap(), signature, types, arguments, *bound, index);
    if (!value) {
      return std::unexpected(value.error());
    }
    values.subspan(index + hidden).front() = *value;
  }
  std::vector<NativeValue> input;
  input.reserve(values.size());
  for (const auto value : values) {
    input.push_back(EncodeNativeValue(value));
  }
  return InvokeNativeTarget(execution, callable.function, input);
}

std::expected<Value, ExecutionStatus> InvokeFunction(NativeExecution& execution,
                                                     Value function,
                                                     const HeapObject& arguments,
                                                     const HeapObject& names) {
  const auto callable = Resolve(execution, function);
  if (!callable) {
    return std::unexpected(callable.error());
  }
  auto* environment = Heap::NativeEnvironment(*function.AsObject());
  if (environment == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto origin = NativeInstance::Borrow(*environment);
  const NativeInstanceScope scope(execution, origin);
  return InvokeDynamic(execution, *callable, arguments, names, DynamicCallStyle::Function);
}

std::expected<Value, ExecutionStatus> ReadMember(NativeExecution& execution,
                                                 const MemberDefinition& member,
                                                 Value receiver,
                                                 Builtin operation) {
  const auto& instance = execution.instance();
  const auto& layouts = instance.layouts();
  const auto resolving = operation == Builtin::DynamicResolveCall;
  std::expected<Value, ExecutionStatus> value;
  switch (member.kind) {
    case MemberKind::Field:
      value = BoxDynamicValue(execution.isolate().heap(),
                              *Heap::GetField(*receiver.AsObject(), member.target),
                              layouts.types.at(*member.type));
      break;
    case MemberKind::Method: {
      const auto& wrapper = *layouts.classes.at(member.target);
      const auto target = wrapper.callable();
      if (!target) {
        std::unreachable();
      }
      if (resolving) {
        return CreatePlan(execution, receiver, target->function);
      }
      auto& heap = execution.isolate().heap();
      auto* callable = heap.AllocateInstance(wrapper, std::span(&receiver, 1));
      if (callable == nullptr) {
        return std::unexpected(ExecutionStatus::OutOfMemory);
      }
      heap.SetNativeEnvironment(*callable, *instance.state().front().AsObject());
      return Value::Object(callable);
    }
    case MemberKind::Property: {
      if (member.target == kInvalidFunctionTarget) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      const std::array arguments{EncodeNativeValue(receiver)};
      value = InvokeNativeTarget(execution, member.target, arguments);
      break;
    }
    default:
      return std::unexpected(ExecutionStatus::TypeError);
  }
  return value && resolving ? CreatePlan(execution, *value) : value;
}

std::expected<Value, ExecutionStatus> WriteMember(NativeExecution& execution,
                                                  const MemberDefinition& member,
                                                  std::span<const Value> arguments) {
  const auto receiver = arguments.front();
  const auto original = arguments.subspan(2).front();
  const auto& instance = execution.instance();
  const auto& types = instance.layouts().types;
  const TypeConstraint* type = nullptr;
  if (member.kind == MemberKind::Field) {
    type = &types.at(*member.type);
  } else if (member.kind == MemberKind::Property && member.setter) {
    type = &types.at(instance.call_signature(*member.setter).parameters.front().type);
  } else {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (!Heap::IsValueOfType(original, *type)) {
    return std::unexpected(ExecutionStatus::InvalidCast);
  }
  auto value = original;
  if (!value.IsNull() && type->instance != nullptr && type->instance->IsEnum()) {
    value = *Heap::GetField(*value.AsObject(), 0);
  }
  if (member.kind == MemberKind::Field) {
    static_cast<void>(
        execution.isolate().heap().SetField(*receiver.AsObject(), member.target, value));
    return original;
  }
  const std::array input{EncodeNativeValue(receiver), EncodeNativeValue(value)};
  const auto result = InvokeNativeTarget(execution, *member.setter, input);
  return result ? std::expected<Value, ExecutionStatus>(original) : std::unexpected(result.error());
}

std::expected<Value, ExecutionStatus> MethodCall(NativeExecution& execution,
                                                 Value plan_value,
                                                 const HeapObject& arguments,
                                                 const HeapObject& names) {
  const auto plan = ReadDynamicCallPlan(plan_value);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  if (plan->method) {
    const ResolvedCallable target{
        .function = *plan->method,
        .implicit = plan->value,
    };
    return InvokeDynamic(execution, target, arguments, names, DynamicCallStyle::Method);
  }
  return InvokeFunction(execution, plan->value, arguments, names);
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteNativeDynamicCall(NativeExecution& execution,
                                                               Builtin builtin,
                                                               std::span<const Value> arguments) {
  if (builtin == Builtin::DynamicArrayPrepare) {
    const auto& instance = execution.instance();
    const auto plan =
        PrepareDynamicArray(execution.isolate(), arguments, instance.layouts().classes,
                            instance.code_identity(), instance.image().functions.size);
    if (!plan) {
      return plan;
    }
    const std::array input{*plan};
    const auto thunk = ReadDynamicArrayOperation(Builtin::DynamicArrayCallback, input);
    if (!thunk) {
      return std::unexpected(thunk.error());
    }
    execution.isolate().heap().SetNativeEnvironment(*thunk->AsObject(),
                                                    *instance.state().front().AsObject());
    return plan;
  }
  if (builtin == Builtin::DynamicArrayInvoke) {
    const auto request = PrepareDynamicArrayInvocation(execution.isolate().heap(), arguments);
    return request
               ? InvokeFunction(execution, request->callback, *request->arguments, *request->names)
               : std::unexpected(request.error());
  }
  if (builtin >= Builtin::DynamicArrayCallback && builtin <= Builtin::DynamicArrayValidate) {
    return ReadDynamicArrayOperation(builtin, arguments);
  }
  const auto first = arguments.front();
  const auto second = arguments.subspan(1).front();
  if (builtin == Builtin::DynamicCall || builtin == Builtin::DynamicMethodCall) {
    const auto names = arguments.subspan(2).front();
    if (!second.IsObject() || !names.IsObject()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    return builtin == Builtin::DynamicCall
               ? InvokeFunction(execution, first, *second.AsObject(), *names.AsObject())
               : MethodCall(execution, first, *second.AsObject(), *names.AsObject());
  }
  if (!second.IsString()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* member = execution.instance().members().Find(first, second.AsString()->value());
  if (member == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return builtin == Builtin::DynamicMemberSet ? WriteMember(execution, *member, arguments)
                                              : ReadMember(execution, *member, first, builtin);
}

}  // namespace vela::runtime
