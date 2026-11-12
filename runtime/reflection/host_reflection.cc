#include <vela/reflection.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/reflection.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_type.h"
#include "runtime/native/host_value.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_type_traits.h"
#include "runtime/reflection/reflection_access.h"
#include "runtime/reflection/reflection_description.h"
#include "runtime/reflection/reflection_index.h"
#include "runtime/value/value.h"
#include "vela/error.h"
#include "vela/handles.h"
#include "vela/types.h"

namespace vela {
namespace internal {

struct ReflectionAccess final {
  static const runtime::HostType& Binding(const TypeDescriptor& type) { return type.binding_; }
  static runtime::ReflectedType Type(const TypeDescriptor& type) { return type.type(); }
  static const TypeInfo<Value>& Owner(const MemberInfo& member) { return member.owner_; }
  static DeclarationId Declaration(const MemberInfo& member) { return member.declaration_; }
  static Result<TypeInfo<Value>> Bind(const runtime::HostType& context,
                                      runtime::ReflectedType type) {
    auto binding = context.WithConstraint(type.constraint);
    if (!binding) {
      return std::unexpected(Error::FromStatus(binding.error()));
    }
    return TypeInfo<Value>(std::move(*binding), type.is_void);
  }
  static MemberInfo Member(const TypeInfo<Value>& owner,
                           const runtime::NativeReflectionMember& member) {
    return {owner, member.declaration};
  }
  static Result<MemberInfo> Member(const runtime::HostType& context,
                                   const runtime::ReflectionIndex& index,
                                   const runtime::NativeReflectionMember& member) {
    auto owner = Bind(context, index.TypeAt(member.declaring_type));
    if (!owner) {
      return std::unexpected(std::move(owner.error()));
    }
    return Member(*owner, member);
  }
};

}  // namespace internal
namespace {

using internal::ReflectionAccess;
using ReflectionKind = reflection::MemberKind;

template <class Action>
std::invoke_result_t<Action, runtime::NativeInstance&> WithProgram(const runtime::HostType& context,
                                                                   const Action& action) {
  const auto state = context.program();
  if (!state) {
    return std::unexpected(Error::FromStatus(state.error()));
  }
  auto instance = runtime::NativeInstance::Borrow(**state);
  const auto* parent = instance.isolate().native_control().state().host_execution;
  if (!runtime::CanReenterNativeProgram(parent, instance.code_identity())) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidEntry));
  }
  return action(instance);
}

template <class Action>
std::invoke_result_t<Action, runtime::NativeInstance*> WithContext(const runtime::HostType& context,
                                                                   const Action& action) {
  if (context.context_identity() == nullptr) {
    const auto status = context.isolate().native_control().Poll();
    if (status != ExecutionStatus::Returned) {
      return std::unexpected(Error::FromStatus(status));
    }
    return action(nullptr);
  }
  return WithProgram(context, [&](auto& instance) { return action(&instance); });
}

template <class Action>
std::invoke_result_t<Action,
                     runtime::NativeInstance&,
                     const runtime::ReflectionIndex&,
                     const runtime::NativeReflectionMember&>
WithMember(const MemberInfo& member, const Action& action) {
  return WithProgram(
      ReflectionAccess::Binding(ReflectionAccess::Owner(member)),
      [&](runtime::NativeInstance& instance) {
        using Output =
            std::invoke_result_t<Action, runtime::NativeInstance&, const runtime::ReflectionIndex&,
                                 const runtime::NativeReflectionMember&>;
        const auto prepared = runtime::PrepareReflectionIndex(instance);
        if (!prepared) {
          return Output(std::unexpected(Error::FromStatus(prepared.error())));
        }
        const auto& index = **prepared;
        const auto resolved = index.ResolveMember(ReflectionAccess::Declaration(member));
        if (!resolved) {
          return Output(std::unexpected(Error::FromStatus(resolved.error())));
        }
        return action(instance, index, **resolved);
      });
}

Result<std::vector<runtime::ReflectedType>> ArgumentTypes(
    const runtime::HostType& context,
    std::span<const TypeDescriptor> arguments) {
  std::vector<runtime::ReflectedType> result;
  result.reserve(arguments.size());
  for (const auto& argument : arguments) {
    if (&argument.isolate() != &context.isolate()) {
      return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
    }
    result.push_back(ReflectionAccess::Type(argument));
  }
  return result;
}

Result<void> DetachedContextStatus(const runtime::HostType& context, ReflectionKind kind) {
  if (kind == ReflectionKind::EnumMember) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::TypeError));
  }
  if (context.constraint().nominal != nullptr) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::ReflectionUnavailable));
  }
  return {};
}

Result<std::optional<MemberInfo>> FindReflectedMember(const TypeDescriptor& owner,
                                                      ReflectionKind kind,
                                                      std::string_view name,
                                                      reflection::MemberQuery query,
                                                      std::span<const TypeDescriptor> arguments) {
  const auto& context = ReflectionAccess::Binding(owner);
  const auto types = ArgumentTypes(context, arguments);
  if (!types) {
    return std::unexpected(types.error());
  }
  return WithContext(
      context, [&](runtime::NativeInstance* instance) -> Result<std::optional<MemberInfo>> {
        if (instance == nullptr) {
          return DetachedContextStatus(context, kind).transform([] {
            return std::optional<MemberInfo>{};
          });
        }
        const auto index = runtime::PrepareReflectionIndex(*instance);
        if (!index) {
          return std::unexpected(Error::FromStatus(index.error()));
        }
        const auto found =
            (*index)->FindMember(ReflectionAccess::Type(owner), kind, name, query, *types);
        if (!found) {
          return std::unexpected(Error::FromStatus(found.error()));
        }
        if (*found == nullptr) {
          return std::nullopt;
        }
        auto member = ReflectionAccess::Member(context, **index, **found);
        if (!member) {
          return std::unexpected(std::move(member.error()));
        }
        return std::optional<MemberInfo>(std::move(*member));
      });
}

Result<std::vector<MemberInfo>> GetMembers(const TypeDescriptor& owner,
                                           ReflectionKind kind,
                                           reflection::MemberQuery query) {
  const auto& context = ReflectionAccess::Binding(owner);
  return WithContext(
      context, [&](runtime::NativeInstance* instance) -> Result<std::vector<MemberInfo>> {
        if (instance == nullptr) {
          return DetachedContextStatus(context, kind).transform([] {
            return std::vector<MemberInfo>{};
          });
        }
        const auto index = runtime::PrepareReflectionIndex(*instance);
        if (!index) {
          return std::unexpected(Error::FromStatus(index.error()));
        }
        std::vector<MemberInfo> members;
        absl::flat_hash_map<std::uint32_t, TypeInfo<Value>> owners;
        std::optional<Error> failure;
        const auto enumerated = (*index)->EnumerateMembers(
            ReflectionAccess::Type(owner), kind, query, [&](const auto& entry) {
              if (failure) {
                return;
              }
              auto found = owners.find(entry.declaring_type);
              if (found == owners.end()) {
                const auto type = (*index)->TypeAt(entry.declaring_type);
                auto declaring = ReflectionAccess::Bind(context, type);
                if (!declaring) {
                  failure = std::move(declaring.error());
                  return;
                }
                found = owners.emplace(entry.declaring_type, std::move(*declaring)).first;
              }
              members.push_back(ReflectionAccess::Member(found->second, entry));
            });
        if (!enumerated) {
          return std::unexpected(Error::FromStatus(enumerated.error()));
        }
        if (failure) {
          return std::unexpected(std::move(*failure));
        }
        return members;
      });
}

Result<std::vector<TypeInfo<Value>>> BindTypes(const runtime::HostType& context,
                                               std::span<const runtime::ReflectedType> types) {
  std::vector<TypeInfo<Value>> output;
  output.reserve(types.size());
  for (const auto type : types) {
    auto result = ReflectionAccess::Bind(context, type);
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
    output.push_back(std::move(*result));
  }
  return output;
}

Result<SignatureInfo> BindSignature(const runtime::HostType& context,
                                    const runtime::ReflectedSignature& signature) {
  auto returned = ReflectionAccess::Bind(context, signature.return_type);
  if (!returned) {
    return std::unexpected(std::move(returned.error()));
  }
  SignatureInfo result{.parameters = {}, .return_type = std::move(*returned)};
  result.parameters.reserve(signature.parameters.size());
  for (const auto& parameter : signature.parameters) {
    auto type = ReflectionAccess::Bind(context, parameter.type);
    if (!type) {
      return std::unexpected(std::move(type.error()));
    }
    result.parameters.push_back({
        .type = std::move(*type),
        .is_optional = parameter.is_optional,
        .is_rest = parameter.is_rest,
    });
  }
  return result;
}

Result<std::optional<MemberInfo>> FindAccessor(const MemberInfo& property, ReflectionKind kind) {
  const auto& context = ReflectionAccess::Binding(ReflectionAccess::Owner(property));
  return WithMember(
      property,
      [&](auto&, const auto& index, const auto& member) -> Result<std::optional<MemberInfo>> {
        const auto* accessor = index.RegisteredAccessor(member, kind);
        if (accessor == nullptr) {
          return std::nullopt;
        }
        auto result = ReflectionAccess::Member(context, index, *accessor);
        if (!result) {
          return std::unexpected(std::move(result.error()));
        }
        return std::optional<MemberInfo>(std::move(*result));
      });
}

Result<bool> CanAccess(const MemberInfo& property, ReflectionKind kind) {
  return WithMember(property, [&](auto&, const auto& index, const auto& member) -> Result<bool> {
    return index.CanAccess(member, kind);
  });
}

template <class Action>
Result<Local<Value>> Execute(HandleScope& scope,
                             const TypeDescriptor& owner,
                             const Action& action) {
  if (&scope.isolate() != &owner.isolate()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  return WithProgram(ReflectionAccess::Binding(owner), [&](runtime::NativeInstance& instance) {
    return internal::ReceiveHostValue<Value>(scope, [&](runtime::Value& output) -> Result<void> {
      runtime::NativeExecution execution(
          instance, instance.isolate().native_control().state().host_execution);
      const auto result = action(execution);
      if (!result) {
        return std::unexpected(internal::CaptureHostError(execution, result.error()));
      }
      internal::StoreHostResult(scope.isolate().heap(), output, *result);
      return {};
    });
  });
}

}  // namespace

bool TypeId::operator==(const TypeId& other) const noexcept {
  return &binding_.isolate() == &other.binding_.isolate() &&
         binding_.constraint() == other.binding_.constraint() && is_void_ == other.is_void_;
}

bool MemberId::operator==(const MemberId& other) const noexcept {
  return &binding_.isolate() == &other.binding_.isolate() &&
         binding_.constraint() == other.binding_.constraint() &&
         binding_.context_identity() == other.binding_.context_identity() &&
         declaration_ == other.declaration_;
}

Result<TypeDescription> TypeDescriptor::Describe() const {
  return WithContext(binding_, [&](runtime::NativeInstance* instance) -> Result<TypeDescription> {
    runtime::ReflectionIndex* index = nullptr;
    if (instance != nullptr) {
      const auto prepared = runtime::PrepareReflectionIndex(*instance);
      if (!prepared) {
        return std::unexpected(Error::FromStatus(prepared.error()));
      }
      index = *prepared;
    }
    auto source = runtime::DescribeReflectedType(type(), index);
    if (!source) {
      return std::unexpected(Error::FromStatus(source.error()));
    }
    auto arguments = BindTypes(binding_, source->type_arguments);
    auto interfaces = BindTypes(binding_, source->interfaces);
    if (!arguments || !interfaces) {
      return std::unexpected(!arguments ? std::move(arguments.error())
                                        : std::move(interfaces.error()));
    }
    TypeDescription result{
        .id = id(),
        .name = std::move(source->name),
        .module = std::move(source->module),
        .kind = source->kind,
        .is_nullable = source->is_nullable,
        .is_registered = source->is_registered,
        .type_arguments = std::move(*arguments),
        .base_type = {},
        .interfaces = std::move(*interfaces),
        .signature = {},
    };
    if (source->base_type) {
      auto base = ReflectionAccess::Bind(binding_, *source->base_type);
      if (!base) {
        return std::unexpected(std::move(base.error()));
      }
      result.base_type = std::move(*base);
    }
    if (source->signature) {
      auto signature = BindSignature(binding_, *source->signature);
      if (!signature) {
        return std::unexpected(std::move(signature.error()));
      }
      result.signature = std::move(*signature);
    }
    return result;
  });
}

Result<bool> TypeDescriptor::IsInstance(HandleScope& scope, Local<Value> value) const {
  const auto& owner = isolate();
  if (&scope.isolate() != &owner || &value.isolate() != &owner) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  return WithContext(binding_, [&](runtime::NativeInstance*) -> Result<bool> {
    return type().IsInstance(internal::HandleAccess::Read(value));
  });
}

Result<bool> TypeDescriptor::IsAssignableFrom(const TypeDescriptor& other) const {
  if (&isolate() != &other.isolate()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  return WithContext(binding_, [&](runtime::NativeInstance*) -> Result<bool> {
    const auto result = type().IsAssignableFrom(other.type());
    return result ? Result<bool>(*result) : std::unexpected(Error::FromStatus(result.error()));
  });
}

Result<std::vector<MemberInfo>> TypeDescriptor::Members(ReflectionKind kind,
                                                        reflection::MemberStorage storage,
                                                        MemberQuery query) const {
  return GetMembers(*this, kind,
                    {.scope = query.scope, .storage = storage, .visibility = query.visibility});
}
Result<std::optional<MemberInfo>> TypeDescriptor::FindMember(
    ReflectionKind kind,
    reflection::MemberStorage storage,
    std::string_view name,
    MemberQuery query,
    std::span<const TypeDescriptor> arguments) const {
  return FindReflectedMember(
      *this, kind, name, {.scope = query.scope, .storage = storage, .visibility = query.visibility},
      arguments);
}
Result<std::vector<StaticFieldInfo>> TypeDescriptor::GetStaticFields(MemberQuery query) const {
  return SelectMembers<StaticFieldInfo>(ReflectionKind::Field, reflection::MemberStorage::Static,
                                        query);
}
Result<std::optional<StaticFieldInfo>> TypeDescriptor::FindStaticField(std::string_view name,
                                                                       MemberQuery query) const {
  return SelectMember<StaticFieldInfo>(ReflectionKind::Field, reflection::MemberStorage::Static,
                                       name, query);
}
Result<std::vector<StaticPropertyInfo>> TypeDescriptor::GetStaticProperties(
    MemberQuery query) const {
  return SelectMembers<StaticPropertyInfo>(ReflectionKind::Property,
                                           reflection::MemberStorage::Static, query);
}
Result<std::optional<StaticPropertyInfo>> TypeDescriptor::FindStaticProperty(
    std::string_view name,
    MemberQuery query) const {
  return SelectMember<StaticPropertyInfo>(ReflectionKind::Property,
                                          reflection::MemberStorage::Static, name, query);
}
Result<std::vector<StaticMethodInfo>> TypeDescriptor::GetStaticMethods(MemberQuery query) const {
  return SelectMembers<StaticMethodInfo>(ReflectionKind::Method, reflection::MemberStorage::Static,
                                         query);
}
Result<std::optional<StaticMethodInfo>> TypeDescriptor::FindStaticMethod(
    std::string_view name,
    MemberQuery query,
    std::span<const TypeDescriptor> arguments) const {
  return SelectMember<StaticMethodInfo>(ReflectionKind::Method, reflection::MemberStorage::Static,
                                        name, query, arguments);
}
Result<std::optional<ConstructorInfo>> TypeDescriptor::GetConstructor() const {
  return SelectMember<ConstructorInfo>(
      ReflectionKind::Constructor, reflection::MemberStorage::Instance,
      reflection::kConstructorName, {.visibility = MemberVisibilityFilter::All});
}
Result<std::vector<EnumMemberInfo>> TypeDescriptor::GetEnumMembers() const {
  return SelectMembers<EnumMemberInfo>(ReflectionKind::EnumMember,
                                       reflection::MemberStorage::Static, {});
}
Result<std::optional<EnumMemberInfo>> TypeDescriptor::FindEnumMember(std::string_view name) const {
  return SelectMember<EnumMemberInfo>(ReflectionKind::EnumMember, reflection::MemberStorage::Static,
                                      name, {});
}

MemberId MemberInfo::id() const {
  return {ReflectionAccess::Binding(owner_), declaration_};
}
Result<MemberDescription> MemberInfo::Describe() const {
  return WithMember(
      *this, [&](auto&, const auto&, const auto& member) -> Result<MemberDescription> {
        return MemberDescription{
            .id = id(),
            .name = std::string(member.name.text()),
            .declaring_type = owner_,
            .kind = static_cast<ReflectionKind>(member.kind),
            .visibility = (member.flags & reflection::kPrivate) != 0 ? MemberVisibility::Private
                                                                     : MemberVisibility::Public,
            .is_static = (member.flags & reflection::kStatic) != 0,
        };
      });
}
Result<TypeInfo<Value>> MemberInfo::GetType() const {
  return WithMember(*this, [&](auto&, const auto& index, const auto& member) {
    return ReflectionAccess::Bind(ReflectionAccess::Binding(owner_), index.TypeAt(member.type));
  });
}
Result<std::vector<ParameterInfo>> MemberInfo::Parameters() const {
  return WithMember(
      *this,
      [&](auto&, const auto& index, const auto& member) -> Result<std::vector<ParameterInfo>> {
        std::vector<ParameterInfo> result;
        result.reserve(member.parameters.size);
        std::size_t position = 0;
        for (const auto& parameter : member.parameters.values()) {
          auto parameter_type = ReflectionAccess::Bind(ReflectionAccess::Binding(owner_),
                                                       index.TypeAt(parameter.type));
          if (!parameter_type) {
            return std::unexpected(std::move(parameter_type.error()));
          }
          result.push_back({
              .name = std::string(parameter.name.text()),
              .position = position,
              .type = std::move(*parameter_type),
              .is_optional = parameter.IsOptional(),
              .is_rest = parameter.IsRest(),
              .has_default = parameter.HasDefault(),
          });
          ++position;
        }
        return result;
      });
}

Result<Local<Value>> MemberInfo::Read(HandleScope& scope, runtime::Value receiver) const {
  return Execute(scope, owner_, [&](auto& execution) {
    return runtime::ReadReflectedMember(execution, declaration_, receiver);
  });
}
Result<void> MemberInfo::Write(HandleScope& scope,
                               runtime::Value receiver,
                               Local<Value> value) const {
  const auto& owner = owner_.isolate();
  if (&value.isolate() != &owner) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  return Execute(scope, owner_,
                 [&](auto& execution) {
                   return runtime::WriteReflectedMember(execution, declaration_, receiver,
                                                        internal::HandleAccess::Read(value))
                       .transform([] { return runtime::Value::Null(); });
                 })
      .transform([](auto) {});
}
Result<Local<Value>> MemberInfo::Call(HandleScope& scope,
                                      runtime::Value receiver,
                                      CallArguments arguments) const {
  const auto& owner = owner_.isolate();
  std::vector<runtime::Value> values;
  std::vector<std::string_view> names;
  values.reserve(arguments.size());
  names.reserve(arguments.size());
  for (const auto& argument : arguments) {
    if (&argument.value.isolate() != &owner) {
      return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
    }
    if (argument.name && argument.name->empty()) {
      return std::unexpected(Error::FromStatus(ExecutionStatus::TypeError));
    }
    values.push_back(internal::HandleAccess::Read(argument.value));
    names.push_back(argument.name.value_or(std::string_view{}));
  }
  return Execute(scope, owner_, [&](auto& execution) {
    return runtime::InvokeReflectedMember(execution, declaration_, receiver, values, names);
  });
}

Result<Local<Value>> StaticFieldInfo::GetValue(HandleScope& scope) const {
  return Read(scope, runtime::Value::Null());
}
Result<void> StaticFieldInfo::SetValue(HandleScope& scope, Local<Value> value) const {
  return Write(scope, runtime::Value::Null(), value);
}
Result<Local<Value>> StaticPropertyInfo::GetValue(HandleScope& scope) const {
  return Read(scope, runtime::Value::Null());
}
Result<void> StaticPropertyInfo::SetValue(HandleScope& scope, Local<Value> value) const {
  return Write(scope, runtime::Value::Null(), value);
}
Result<bool> PropertyDescriptor::CanRead() const {
  return CanAccess(*this, ReflectionKind::Getter);
}
Result<bool> PropertyDescriptor::CanWrite() const {
  return CanAccess(*this, ReflectionKind::Setter);
}
Result<std::optional<MemberInfo>> PropertyDescriptor::Accessor(ReflectionKind kind) const {
  return FindAccessor(*this, kind);
}
Result<std::optional<StaticMethodInfo>> StaticPropertyInfo::GetAccessor(ReflectionKind kind) const {
  return Accessor(kind).transform([](auto member) -> std::optional<StaticMethodInfo> {
    if (!member) {
      return std::nullopt;
    }
    return StaticMethodInfo(std::move(*member));
  });
}
Result<std::optional<StaticMethodInfo>> StaticPropertyInfo::GetGetter() const {
  return GetAccessor(ReflectionKind::Getter);
}
Result<std::optional<StaticMethodInfo>> StaticPropertyInfo::GetSetter() const {
  return GetAccessor(ReflectionKind::Setter);
}
Result<std::vector<ParameterInfo>> MethodDescriptor::GetParameters() const {
  return Parameters();
}
Result<TypeInfo<Value>> MethodDescriptor::GetReturnType() const {
  return GetType();
}
Result<std::vector<TypeInfo<Value>>> MethodDescriptor::GetTypeArguments() const {
  return WithMember(*this, [&](auto&, const auto& index, const auto& member) {
    std::vector<runtime::ReflectedType> types;
    types.reserve(member.type_arguments.size);
    for (const auto type : member.type_arguments.values()) {
      types.push_back(index.TypeAt(type));
    }
    return BindTypes(ReflectionAccess::Binding(ReflectionAccess::Owner(*this)), types);
  });
}
Result<bool> MethodDescriptor::IsAsync() const {
  return WithMember(*this, [](auto&, const auto&, const auto& member) -> Result<bool> {
    return (member.flags & reflection::kAsync) != 0;
  });
}
Result<Local<Value>> StaticMethodInfo::Invoke(HandleScope& scope, CallArguments arguments) const {
  return Call(scope, runtime::Value::Null(), arguments);
}
Result<std::vector<ParameterInfo>> ConstructorInfo::GetParameters() const {
  return Parameters();
}
Result<Local<Value>> ConstructorInfo::Invoke(HandleScope& scope, CallArguments arguments) const {
  return Call(scope, runtime::Value::Null(), arguments);
}
Result<std::int64_t> EnumMemberInfo::Ordinal() const {
  return WithMember(*this, [](auto&, const auto&, const auto& member) -> Result<std::int64_t> {
    return member.target;
  });
}
Result<Local<Value>> EnumMemberInfo::GetValue(HandleScope& scope) const {
  return Execute(scope, ReflectionAccess::Owner(*this), [&](auto& execution) {
    return runtime::ReadReflectedEnumMember(execution, ReflectionAccess::Declaration(*this));
  });
}

Result<Reflection> Reflection::Bind(runtime::NativeInstance& instance) {
  auto binding = runtime::HostType::Bind(instance, runtime::TypeConstraint{.dynamic = true});
  if (!binding) {
    return std::unexpected(Error::FromStatus(binding.error()));
  }
  return Reflection(std::move(*binding));
}
Result<Reflection> Reflection::Bind(runtime::NativeDispatch& dispatch) {
  auto binding = runtime::HostType::Bind(dispatch, runtime::internal::NativeTypeOf<Value>());
  if (!binding) {
    return std::unexpected(Error::FromStatus(binding.error()));
  }
  return Reflection(std::move(*binding));
}
Result<TypeInfo<Value>> Reflection::TypeOf(HandleScope& scope, Local<Value> value) const {
  const auto& owner = binding_.isolate();
  if (&scope.isolate() != &owner || &value.isolate() != &owner) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  return WithProgram(binding_, [&](auto&) {
    return ReflectionAccess::Bind(
        binding_, {.constraint = runtime::Heap::TypeOf(internal::HandleAccess::Read(value))});
  });
}
Result<std::vector<TypeInfo<Value>>> Reflection::GetTypes(std::string_view module) const {
  return WithProgram(binding_, [&](auto& instance) -> Result<std::vector<TypeInfo<Value>>> {
    const auto index = runtime::PrepareReflectionIndex(instance);
    if (!index) {
      return std::unexpected(Error::FromStatus(index.error()));
    }
    std::vector<runtime::ReflectedType> types;
    (*index)->EnumerateTypes(
        module, [&](const auto& type) { types.push_back((*index)->TypeAt(type.type)); });
    return BindTypes(binding_, types);
  });
}
Result<std::optional<TypeInfo<Value>>> Reflection::FindType(
    std::string_view module,
    std::string_view name,
    std::span<const TypeDescriptor> arguments) const {
  const auto types = ArgumentTypes(binding_, arguments);
  if (!types) {
    return std::unexpected(types.error());
  }
  return WithProgram(binding_, [&](auto& instance) -> Result<std::optional<TypeInfo<Value>>> {
    const auto index = runtime::PrepareReflectionIndex(instance);
    if (!index) {
      return std::unexpected(Error::FromStatus(index.error()));
    }
    const auto* found = (*index)->FindType(module, name, *types);
    if (found == nullptr) {
      return std::nullopt;
    }
    auto type = ReflectionAccess::Bind(binding_, (*index)->TypeAt(found->type));
    if (!type) {
      return std::unexpected(std::move(type.error()));
    }
    return std::optional<TypeInfo<Value>>(std::move(*type));
  });
}
Result<std::uint64_t> Reflection::Revision() const {
  return WithProgram(binding_, [](auto& instance) -> Result<std::uint64_t> {
    return instance.current_revision();
  });
}

}  // namespace vela
