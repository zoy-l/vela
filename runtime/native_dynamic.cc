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
#include "runtime/native_dispatch.h"
#include "runtime/native_instance.h"
#include "runtime/native_program_state.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "runtime/value_buffer.h"
#include "runtime/value_operations.h"

namespace vela::runtime {
namespace {

std::expected<Value, ExecutionStatus> CreatePlan(
    NativeExecution& execution,
    Value value,
    std::optional<std::uint32_t> target = std::nullopt) {
  if (!target) {
    if (const auto resolved = ResolveNativeCallable(execution.instance(), value); !resolved) {
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
  const RootRange roots(execution.isolate().heap(), values.data(), values.size(),
                        RootOwner::Runtime);
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
  const auto callable = ResolveNativeCallable(execution.instance(), function);
  if (!callable) {
    return std::unexpected(callable.error());
  }
  auto origin = NativeInstance::Borrow(*callable->state);
  const NativeInstanceScope scope(execution, origin);
  return InvokeDynamic(execution, callable->target, arguments, names, DynamicCallStyle::Function);
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
    case MemberKind::Field: {
      const auto& type = member.type;
      if (!type) {
        std::unreachable();
      }
      auto& object = *receiver.AsObject();
      const auto* layout = Heap::ClassOf(object);
      auto* dispatch = Heap::NativeProgram(*instance.state().front().AsObject()).dispatch;
      const auto loaded =
          layout->storage() == ClassStorage::Reloadable
              ? dispatch->LoadField(execution, object,
                                    layout->field_declarations().subspan(member.target).front())
              : std::expected<Value, ExecutionStatus>(*Heap::GetField(object, member.target));
      if (!loaded) {
        return std::unexpected(loaded.error());
      }
      value = BoxDynamicValue(execution.isolate().heap(), *loaded, layouts.types.at(*type));
      break;
    }
    case MemberKind::Method: {
      const auto& wrapper = *layouts.classes.at(member.target);
      const auto* const target = wrapper.callable();
      if (target == nullptr) {
        std::unreachable();
      }
      if (resolving &&
          Heap::NativeProgram(*instance.state().front().AsObject()).dispatch == nullptr) {
        return CreatePlan(execution, receiver, target->function);
      }
      const auto callable = AllocateNativeCallable(execution, wrapper, std::span(&receiver, 1));
      if (!callable) {
        return std::unexpected(callable.error());
      }
      const auto captured = Value::Object(*callable);
      return resolving ? CreatePlan(execution, captured)
                       : std::expected<Value, ExecutionStatus>(captured);
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
  const auto setter = member.setter;
  const TypeConstraint* type = nullptr;
  if (member.kind == MemberKind::Field) {
    const auto& field_type = member.type;
    if (!field_type) {
      std::unreachable();
    }
    type = &types.at(*field_type);
  } else if (member.kind == MemberKind::Property && setter) {
    type = &types.at(instance.call_signature(*setter).parameters.front().type);
  } else {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (!Heap::IsValueOfType(original, *type)) {
    return std::unexpected(ExecutionStatus::InvalidCast);
  }
  auto value = original;
  if (!value.IsNull() && type->nominal != nullptr && type->nominal->IsEnum()) {
    value = *Heap::GetField(*value.AsObject(), 0);
  }
  if (member.kind == MemberKind::Field) {
    auto& object = *receiver.AsObject();
    const auto* layout = Heap::ClassOf(object);
    if (layout->storage() == ClassStorage::Reloadable) {
      auto* dispatch = Heap::NativeProgram(*instance.state().front().AsObject()).dispatch;
      const auto stored = dispatch->StoreField(
          object, layout->field_declarations().subspan(member.target).front(), value);
      return stored ? std::expected<Value, ExecutionStatus>(original)
                    : std::unexpected(stored.error());
    }
    static_cast<void>(
        execution.isolate().heap().SetField(*receiver.AsObject(), member.target, value));
    return original;
  }
  const std::array input{EncodeNativeValue(receiver), EncodeNativeValue(value)};
  if (!setter) {
    std::unreachable();
  }
  const auto result = InvokeNativeTarget(execution, *setter, input);
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
  const auto& method = plan->method;
  if (method) {
    const ResolvedCallable target{
        .function = *method,
        .implicit = plan->value,
    };
    return InvokeDynamic(execution, target, arguments, names, DynamicCallStyle::Method);
  }
  return InvokeFunction(execution, plan->value, arguments, names);
}

std::expected<Value, ExecutionStatus> AccessMember(NativeExecution& execution,
                                                   Builtin builtin,
                                                   std::span<const Value> arguments) {
  const auto receiver = arguments.front();
  const auto name = arguments.subspan(1).front();
  if (!name.IsString()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* member = execution.instance().members().Find(receiver, name.AsString()->value());
  if (member == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return builtin == Builtin::DynamicMemberSet ? WriteMember(execution, *member, arguments)
                                              : ReadMember(execution, *member, receiver, builtin);
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteNativeDynamicCall(NativeExecution& execution,
                                                               Builtin builtin,
                                                               std::span<const Value> arguments) {
  if (builtin == Builtin::DynamicArrayPrepare) {
    const auto& instance = execution.instance();
    const auto callback_value = arguments.subspan(1).front();
    const auto callback = ResolveNativeCallable(instance, callback_value);
    if (!callback) {
      return std::unexpected(callback.error());
    }
    const auto* signature = Heap::ClassOf(*callback_value.AsObject())->callable_type();
    if (signature == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto plan =
        PrepareDynamicArray(execution.isolate(), arguments, instance.layouts().classes, *signature);
    if (!plan) {
      return plan;
    }
    const std::array input{*plan};
    const auto thunk = ReadDynamicArrayOperation(Builtin::DynamicArrayCallback, input);
    if (!thunk) {
      return std::unexpected(thunk.error());
    }
    auto& thunk_object = *thunk->AsObject();
    execution.isolate().heap().SetNativeEnvironment(
        thunk_object, *instance.state().front().AsObject(),
        Heap::ClassOf(thunk_object)->callable()->function);
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
  auto* dispatch = Heap::NativeProgram(*execution.instance().state().front().AsObject()).dispatch;
  if (dispatch != nullptr) {
    auto current = NativeInstance::Borrow(dispatch->current_state());
    const NativeInstanceScope scope(execution, current);
    return AccessMember(execution, builtin, arguments);
  }
  return AccessMember(execution, builtin, arguments);
}

}  // namespace vela::runtime
