#include "runtime/reflection/reflection_access.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/reflection.h"
#include "runtime/dynamic/dynamic_call.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_dynamic.h"
#include "runtime/native/native_global_internal.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_program_state.h"
#include "runtime/reflection/reflection_index.h"
#include "runtime/value/callable.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {
namespace {

using reflection::MemberKind;

std::expected<const NativeReflectionMember*, ExecutionStatus> Resolve(NativeExecution& execution,
                                                                      DeclarationId declaration) {
  const auto index = PrepareReflectionIndex(execution.instance());
  if (!index) {
    return std::unexpected(index.error());
  }
  return (*index)->ResolveMember(declaration);
}

std::expected<void, ExecutionStatus> CheckAccess(const NativeReflectionMember& member) {
  return (member.flags & reflection::kPrivate) != 0
             ? std::expected<void, ExecutionStatus>(
                   std::unexpected(ExecutionStatus::ReflectionAccessDenied))
             : std::expected<void, ExecutionStatus>{};
}

std::expected<void, ExecutionStatus> CheckReceiver(NativeExecution& execution,
                                                   const NativeReflectionMember& member,
                                                   Value receiver) {
  if ((member.flags & reflection::kStatic) != 0 ||
      member.kind == static_cast<std::uint32_t>(MemberKind::Constructor)) {
    return receiver.IsNull() ? std::expected<void, ExecutionStatus>{}
                             : std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& index = *execution.instance().program().reflection;
  return !receiver.IsNull() &&
                 Heap::IsValueOfType(receiver, index.TypeAt(member.declaring_type).constraint)
             ? std::expected<void, ExecutionStatus>{}
             : std::unexpected(ExecutionStatus::InvalidCast);
}

std::expected<std::uint32_t, ExecutionStatus> CallableTarget(NativeExecution& execution,
                                                             const NativeReflectionMember& member,
                                                             Value receiver) {
  const auto& instance = execution.instance();
  const auto owner = instance.program().reflection->TypeAt(member.declaring_type);
  if (owner.constraint.interface == nullptr) {
    return member.target;
  }
  const auto& layout = *Heap::ClassOf(*receiver.AsObject());
  if (const auto* dispatch = instance.dispatch()) {
    const auto resolved = dispatch->ResolveInterface(layout.nominal_type(), member.declaration);
    return resolved ? std::expected<std::uint32_t, ExecutionStatus>(resolved->function)
                    : std::unexpected(resolved.error());
  }
  return ResolveInterfaceTarget(member.target, receiver, instance.image().functions.size);
}

enum class ReflectedAccess : std::uint8_t { Value, Callable };

std::expected<void, ExecutionStatus> ValidateReflectedAccess(NativeExecution& execution,
                                                             const NativeReflectionMember& member,
                                                             Value receiver,
                                                             ReflectedAccess access) {
  const auto kind = static_cast<MemberKind>(member.kind);
  if (access == ReflectedAccess::Value) {
    // 属性由实际选择的访问器检查权限和接收者，保留缺失访问器的错误顺序。
    if (kind == MemberKind::Property) {
      return {};
    }
    if (kind != MemberKind::Field) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  } else if (!reflection::IsCallable(kind)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (const auto checked = CheckAccess(member); !checked) {
    return std::unexpected(checked.error());
  }
  if (const auto checked = CheckReceiver(execution, member, receiver); !checked) {
    return std::unexpected(checked.error());
  }
  return {};
}

// 调用方在解析成员前已保根 receiver 与 values，成员借用当前执行实例的索引。
std::expected<Value, ExecutionStatus> InvokeResolvedMember(
    NativeExecution& execution,
    const NativeReflectionMember& member,
    Value receiver,
    std::span<const Value> values,
    std::span<const std::string_view> names) {
  const auto validated =
      ValidateReflectedAccess(execution, member, receiver, ReflectedAccess::Callable);
  if (!validated) {
    return std::unexpected(validated.error());
  }
  const auto constructor = member.kind == static_cast<std::uint32_t>(MemberKind::Constructor);
  if (constructor && std::ranges::any_of(names, [](auto name) { return name.empty(); })) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& instance = execution.instance();
  const auto prepared = instance.program().reflection->Signature(member);
  if (!prepared) {
    return std::unexpected(prepared.error());
  }
  const auto& signature = **prepared;
  const auto bound = BindDynamicArguments(signature, names, DynamicCallStyle::Method);
  if (!bound) {
    return std::unexpected(bound.error());
  }
  const auto target = CallableTarget(execution, member, receiver);
  if (!target) {
    return std::unexpected(target.error());
  }
  const auto hidden = !constructor && (member.flags & reflection::kStatic) == 0 ? 1U : 0U;
  // 普通构造工厂的 presence 参数独立于逻辑签名；省略不等于显式 null。
  return InvokeBoundNativeTarget(
      execution, *target, signature, *bound, values,
      hidden != 0 ? std::optional(receiver) : std::nullopt,
      constructor ? NativeArgumentPresence::Included : NativeArgumentPresence::Absent);
}

}  // namespace

std::expected<Value, ExecutionStatus> InvokeReflectedMember(
    NativeExecution& execution,
    DeclarationId declaration,
    Value receiver,
    std::span<const Value> values,
    std::span<const std::string_view> names) {
  auto& heap = execution.isolate().heap();
  const RootRange receiver_root(heap, &receiver, 1, std::nullopt);
  const RootRange arguments_root(heap, values.data(), values.size(), std::nullopt);
  const auto resolved = Resolve(execution, declaration);
  return resolved ? InvokeResolvedMember(execution, **resolved, receiver, values, names)
                  : std::unexpected(resolved.error());
}

std::expected<Value, ExecutionStatus> ReadReflectedMember(NativeExecution& execution,
                                                          DeclarationId declaration,
                                                          Value receiver) {
  auto& heap = execution.isolate().heap();
  const RootRange root(heap, &receiver, 1, std::nullopt);
  const auto resolved = Resolve(execution, declaration);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  const auto& member = **resolved;
  if (const auto validated =
          ValidateReflectedAccess(execution, member, receiver, ReflectedAccess::Value);
      !validated) {
    return std::unexpected(validated.error());
  }
  if (member.kind == static_cast<std::uint32_t>(MemberKind::Property)) {
    const auto getter =
        execution.instance().program().reflection->ResolveAccessor(member, MemberKind::Getter);
    return getter ? InvokeResolvedMember(execution, **getter, receiver, {}, {})
                  : std::unexpected(getter.error());
  }
  auto& instance = execution.instance();
  const auto& type = instance.layouts().types[member.type];
  if ((member.flags & reflection::kStatic) == 0) {
    return ReadNativeField(execution, receiver, member.target, member.declaration, type);
  }
  auto* dispatch = instance.dispatch();
  std::expected<Value, ExecutionStatus> value;
  if (dispatch != nullptr) {
    value = dispatch->LoadGlobal(execution, member.declaration,
                                 instance.program().global_cache(member.target));
  } else {
    auto& program = instance.program();
    value = program.global_state(member.target) == NativeProgramState::GlobalState::Ready
                ? std::expected<Value, ExecutionStatus>(Heap::ValueSlots(
                      *program.values[NativeProgramState::kGlobals].AsObject())[member.target])
                : instance.InitializeGlobal(execution, member.target);
  }
  if (!value) {
    return value;
  }
  return BoxDynamicValue(heap, *value, type);
}

std::expected<void, ExecutionStatus> WriteReflectedMember(NativeExecution& execution,
                                                          DeclarationId declaration,
                                                          Value receiver,
                                                          Value original) {
  auto& heap = execution.isolate().heap();
  const std::array inputs{receiver, original};
  const RootRange roots(heap, inputs.data(), inputs.size(), std::nullopt);
  const auto resolved = Resolve(execution, declaration);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  const auto& member = **resolved;
  if (const auto validated =
          ValidateReflectedAccess(execution, member, receiver, ReflectedAccess::Value);
      !validated) {
    return std::unexpected(validated.error());
  }
  if (member.kind == static_cast<std::uint32_t>(MemberKind::Property)) {
    const auto setter =
        execution.instance().program().reflection->ResolveAccessor(member, MemberKind::Setter);
    if (!setter) {
      return std::unexpected(setter.error());
    }
    const std::array values{original};
    const std::array<std::string_view, 1> names{};
    return InvokeResolvedMember(execution, **setter, receiver, values, names).transform([](Value) {
    });
  }
  auto& instance = execution.instance();
  const auto& type = instance.layouts().types[member.type];
  auto* dispatch = instance.dispatch();
  if ((member.flags & reflection::kStatic) != 0) {
    if (!Heap::IsValueOfType(original, type)) {
      return std::unexpected(ExecutionStatus::InvalidCast);
    }
    const auto value = UnboxDynamicValue(original, type);
    if ((instance.image().global_flags.values()[member.target] &
         static_cast<std::uint32_t>(GlobalFlag::Constant)) != 0) {
      return std::unexpected(ExecutionStatus::ReflectionAccessDenied);
    }
    if (dispatch != nullptr) {
      return dispatch->StoreGlobal(member.declaration, value,
                                   instance.program().global_cache(member.target));
    }
    auto& program = instance.program();
    auto& storage = *program.values[NativeProgramState::kGlobals].AsObject();
    internal::StoreNativeGlobal(heap, storage, member.target, program.global_state(member.target),
                                value);
    return {};
  }
  return WriteNativeField(execution, receiver, member.target, member.declaration, original, type)
      .transform([](Value) {});
}

std::expected<Value, ExecutionStatus> ReadReflectedEnumMember(NativeExecution& execution,
                                                              DeclarationId declaration) {
  const auto resolved = Resolve(execution, declaration);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  const auto& member = **resolved;
  if (member.kind != static_cast<std::uint32_t>(MemberKind::EnumMember)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return BoxDynamicValue(execution.isolate().heap(), Value::Integer(member.target),
                         execution.instance().layouts().types[member.type]);
}

}  // namespace vela::runtime
