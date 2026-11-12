#include "runtime/native/native_dynamic.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "base/builtin.h"
#include "base/callable.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/member.h"
#include "runtime/dynamic/dynamic_array.h"
#include "runtime/dynamic/dynamic_call.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_program_state.h"
#include "runtime/strings/string.h"
#include "runtime/value/callable.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {
namespace {

DeclarationId FieldDeclaration(const ClassLayout& layout, std::uint32_t slot) {
  return layout.storage() == ClassStorage::Reloadable ? layout.field_declarations()[slot]
                                                      : DeclarationId{};
}

template <class Action>
auto InCurrentInstance(NativeExecution& execution, const Action& action) {
  if (auto* dispatch = execution.instance().dispatch()) {
    auto current = NativeInstance::Borrow(dispatch->current_state());
    const NativeInstanceScope scope(execution, current);
    return action();
  }
  return action();
}

const MemberDefinition* FindMember(const NativeInstance& instance,
                                   Value receiver,
                                   std::string_view name,
                                   const TypeConstraint& contract) {
  const auto& members = instance.members();
  return contract.interface != nullptr ? members.Find(contract, name)
                                       : members.Find(receiver, name);
}

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
                                                    const HeapObject* names,
                                                    DynamicCallStyle style) {
  const auto& instance = execution.instance();
  const auto& signature = instance.call_signature(callable.function);
  const auto bound = BindDynamicArguments(signature, callable, arguments, names, style);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  return InvokeBoundNativeTarget(execution, callable.function, signature, *bound, arguments,
                                 callable.implicit);
}

std::expected<Value, ExecutionStatus> InvokeFunction(NativeExecution& execution,
                                                     Value function,
                                                     const HeapObject& arguments,
                                                     const HeapObject* names) {
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
      assert(type.has_value());
      auto& object = *receiver.AsObject();
      const auto* layout = Heap::ClassOf(object);
      const auto declaration = FieldDeclaration(*layout, member.target);
      value =
          ReadNativeField(execution, receiver, member.target, declaration, layouts.types.at(*type));
      break;
    }
    case MemberKind::Method: {
      const auto& wrapper = *layouts.classes.at(member.target);
      const auto* const target = wrapper.callable();
      if (resolving && instance.dispatch() == nullptr) {
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
  }
  return value && resolving ? CreatePlan(execution, *value) : value;
}

std::expected<Value, ExecutionStatus> WriteMember(NativeExecution& execution,
                                                  Value receiver,
                                                  const MemberDefinition& member,
                                                  Value original) {
  const auto& instance = execution.instance();
  const auto& types = instance.layouts().types;
  const auto setter = member.setter;
  const TypeConstraint* type = nullptr;
  if (member.kind == MemberKind::Field) {
    const auto& field_type = member.type;
    assert(field_type.has_value());
    type = &types.at(*field_type);
  } else if (member.kind == MemberKind::Property && setter) {
    type = &types.at(instance.call_signature(*setter).parameters.front().type);
  } else {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (member.kind == MemberKind::Field) {
    auto& object = *receiver.AsObject();
    const auto* layout = Heap::ClassOf(object);
    const auto declaration = FieldDeclaration(*layout, member.target);
    return WriteNativeField(execution, receiver, member.target, declaration, original, *type);
  }
  if (!Heap::IsValueOfType(original, *type)) {
    return std::unexpected(ExecutionStatus::InvalidCast);
  }
  const auto value = UnboxDynamicValue(original, *type);
  assert(setter.has_value());
  const std::array input{EncodeNativeValue(receiver), EncodeNativeValue(value)};
  const auto result = InvokeNativeTarget(execution, *setter, input);
  return result ? std::expected<Value, ExecutionStatus>(original) : std::unexpected(result.error());
}

std::expected<Value, ExecutionStatus> MethodCall(NativeExecution& execution,
                                                 Value plan_value,
                                                 const HeapObject& arguments,
                                                 const HeapObject* names) {
  const auto plan = ReadDynamicCallPlan(plan_value);
  const auto& method = plan.method;
  if (method) {
    const ResolvedCallable target{
        .function = *method,
        .implicit = plan.value,
    };
    return InvokeDynamic(execution, target, arguments, names, DynamicCallStyle::Method);
  }
  return InvokeFunction(execution, plan.value, arguments, names);
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
  return builtin == Builtin::DynamicMemberSet
             ? WriteMember(execution, receiver, *member, arguments[2])
             : ReadMember(execution, *member, receiver, builtin);
}

}  // namespace

std::expected<Value, ExecutionStatus> ReadNativeField(NativeExecution& execution,
                                                      Value receiver,
                                                      std::uint32_t field,
                                                      DeclarationId declaration,
                                                      const TypeConstraint& type) {
  auto& object = *receiver.AsObject();
  const auto* layout = Heap::ClassOf(object);
  const auto loaded =
      layout->storage() == ClassStorage::Reloadable
          ? execution.instance().dispatch()->LoadField(execution, object, declaration)
          : std::expected<Value, ExecutionStatus>(Heap::LoadField(object, field));
  if (!loaded) {
    return std::unexpected(loaded.error());
  }
  return BoxDynamicValue(execution.isolate().heap(), *loaded, type);
}

std::expected<Value, ExecutionStatus> WriteNativeField(NativeExecution& execution,
                                                       Value receiver,
                                                       std::uint32_t field,
                                                       DeclarationId declaration,
                                                       Value original,
                                                       const TypeConstraint& type) {
  if (!Heap::IsValueOfType(original, type)) {
    return std::unexpected(ExecutionStatus::InvalidCast);
  }
  const auto value = UnboxDynamicValue(original, type);
  auto& object = *receiver.AsObject();
  const auto* layout = Heap::ClassOf(object);
  if (layout->storage() == ClassStorage::Reloadable) {
    const auto stored = execution.instance().dispatch()->StoreField(object, declaration, value);
    return stored ? std::expected<Value, ExecutionStatus>(original)
                  : std::unexpected(stored.error());
  }
  return execution.isolate().heap().SetField(object, field, value).transform([original] {
    return original;
  });
}

std::expected<Value, ExecutionStatus> ReadNativeMember(NativeExecution& execution,
                                                       Value receiver,
                                                       std::string_view name,
                                                       const TypeConstraint& contract) {
  const auto read = [&] -> std::expected<Value, ExecutionStatus> {
    const auto* member = FindMember(execution.instance(), receiver, name, contract);
    return member != nullptr ? ReadMember(execution, *member, receiver, Builtin::DynamicMemberGet)
                             : std::unexpected(ExecutionStatus::InvalidField);
  };
  return InCurrentInstance(execution, read);
}

std::expected<void, ExecutionStatus> WriteNativeMember(NativeExecution& execution,
                                                       Value receiver,
                                                       std::string_view name,
                                                       Value value,
                                                       const TypeConstraint& contract) {
  const auto write = [&] -> std::expected<void, ExecutionStatus> {
    const auto* member = FindMember(execution.instance(), receiver, name, contract);
    if (member == nullptr) {
      return std::unexpected(ExecutionStatus::InvalidField);
    }
    return WriteMember(execution, receiver, *member, value).transform([](Value) {});
  };
  return InCurrentInstance(execution, write);
}

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
      return std::unexpected(plan.error());
    }
    auto& thunk_object = *plan->thunk;
    execution.isolate().heap().SetNativeEnvironment(
        thunk_object, *instance.state().front().AsObject(),
        Heap::ClassOf(thunk_object)->callable()->function);
    return plan->plan;
  }
  if (builtin == Builtin::DynamicArrayInvoke) {
    const auto request = PrepareDynamicArrayInvocation(execution.isolate().heap(), arguments);
    return request ? InvokeFunction(execution, request->callback, *request->arguments, nullptr)
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
               ? InvokeFunction(execution, first, *second.AsObject(), names.AsObject())
               : MethodCall(execution, first, *second.AsObject(), names.AsObject());
  }
  return InCurrentInstance(execution, [&] { return AccessMember(execution, builtin, arguments); });
}

}  // namespace vela::runtime
