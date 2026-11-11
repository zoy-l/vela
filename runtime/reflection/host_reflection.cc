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
  static const runtime::HostType& Binding(const TypeInfo& type) { return type.binding_; }
  static runtime::ReflectedType Type(const TypeInfo& type) { return type.type(); }
  static const TypeInfo& Owner(const MemberInfo& member) { return member.owner_; }
  static DeclarationId Declaration(const MemberInfo& member) { return member.declaration_; }
  static Result<TypeInfo> Bind(const runtime::HostType& context, runtime::ReflectedType type) {
    auto binding = context.WithConstraint(type.constraint);
    if (!binding) {
      return std::unexpected(Error::FromStatus(binding.error()));
    }
    return TypeInfo(std::move(*binding), type.is_void);
  }
  template <class T>
  static T Member(const TypeInfo& owner, const runtime::NativeReflectionMember& member) {
    return T(owner, member.declaration);
  }
  template <class T>
  static Result<T> Member(const runtime::HostType& context,
                          const runtime::ReflectionIndex& index,
                          const runtime::NativeReflectionMember& member) {
    auto owner = Bind(context, index.TypeAt(member.declaring_type));
    if (!owner) {
      return std::unexpected(std::move(owner.error()));
    }
    return Member<T>(*owner, member);
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

Result<std::vector<runtime::ReflectedType>> ArgumentTypes(const runtime::HostType& context,
                                                          std::span<const TypeInfo> arguments) {
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

template <class T>
Result<std::optional<T>> FindMember(const TypeInfo& owner,
                                    ReflectionKind kind,
                                    std::string_view name,
                                    MemberQuery query,
                                    std::span<const TypeInfo> arguments = {}) {
  const auto& context = ReflectionAccess::Binding(owner);
  const auto types = ArgumentTypes(context, arguments);
  if (!types) {
    return std::unexpected(types.error());
  }
  return WithContext(context, [&](runtime::NativeInstance* instance) -> Result<std::optional<T>> {
    if (instance == nullptr) {
      return DetachedContextStatus(context, kind).transform([] { return std::optional<T>{}; });
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
    auto member = ReflectionAccess::Member<T>(context, **index, **found);
    if (!member) {
      return std::unexpected(std::move(member.error()));
    }
    return std::optional<T>(std::move(*member));
  });
}

template <class T>
Result<std::vector<T>> GetMembers(const TypeInfo& owner, ReflectionKind kind, MemberQuery query) {
  const auto& context = ReflectionAccess::Binding(owner);
  return WithContext(context, [&](runtime::NativeInstance* instance) -> Result<std::vector<T>> {
    if (instance == nullptr) {
      return DetachedContextStatus(context, kind).transform([] { return std::vector<T>{}; });
    }
    const auto index = runtime::PrepareReflectionIndex(*instance);
    if (!index) {
      return std::unexpected(Error::FromStatus(index.error()));
    }
    std::vector<T> members;
    absl::flat_hash_map<std::uint32_t, TypeInfo> owners;
    std::optional<Error> failure;
    const auto enumerated = (*index)->EnumerateMembers(
        ReflectionAccess::Type(owner), kind, query, [&](const auto& entry) {
          if (failure) {
            return;
          }
          auto found = owners.find(entry.declaring_type);
          if (found == owners.end()) {
            const auto type = (*index)->TypeAt(entry.declaring_type);
            auto declaring = type == ReflectionAccess::Type(owner)
                                 ? Result<TypeInfo>(owner)
                                 : ReflectionAccess::Bind(context, type);
            if (!declaring) {
              failure = std::move(declaring.error());
              return;
            }
            found = owners.emplace(entry.declaring_type, std::move(*declaring)).first;
          }
          members.push_back(ReflectionAccess::Member<T>(found->second, entry));
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

Result<std::vector<TypeInfo>> BindTypes(const runtime::HostType& context,
                                        std::span<const runtime::ReflectedType> types) {
  std::vector<TypeInfo> output;
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

Result<std::optional<MethodInfo>> GetAccessor(const PropertyInfo& property, ReflectionKind kind) {
  const auto& context = ReflectionAccess::Binding(ReflectionAccess::Owner(property));
  return WithMember(
      property,
      [&](auto&, const auto& index, const auto& member) -> Result<std::optional<MethodInfo>> {
        const auto* accessor = index.RegisteredAccessor(member, kind);
        if (accessor == nullptr) {
          return std::nullopt;
        }
        auto result = ReflectionAccess::Member<MethodInfo>(context, index, *accessor);
        if (!result) {
          return std::unexpected(std::move(result.error()));
        }
        return std::optional<MethodInfo>(std::move(*result));
      });
}

Result<bool> CanAccess(const PropertyInfo& property, ReflectionKind kind) {
  return WithMember(property, [&](auto&, const auto& index, const auto& member) -> Result<bool> {
    return index.CanAccess(member, kind);
  });
}

template <class Action>
Result<Local<Value>> Execute(HandleScope& scope, const TypeInfo& owner, const Action& action) {
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

Result<TypeDescription> TypeInfo::Describe() const {
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

Result<bool> TypeInfo::IsInstance(HandleScope& scope, Local<Value> value) const {
  const auto& owner = isolate();
  if (&scope.isolate() != &owner || &value.isolate() != &owner) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  return WithContext(binding_, [&](runtime::NativeInstance*) -> Result<bool> {
    return type().IsInstance(internal::HandleAccess::Read(value));
  });
}

Result<bool> TypeInfo::IsAssignableFrom(const TypeInfo& other) const {
  if (&isolate() != &other.isolate()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  return WithContext(binding_, [&](runtime::NativeInstance*) -> Result<bool> {
    const auto result = type().IsAssignableFrom(other.type());
    return result ? Result<bool>(*result) : std::unexpected(Error::FromStatus(result.error()));
  });
}

Result<std::vector<FieldInfo>> TypeInfo::GetFields(MemberQuery query) const {
  return GetMembers<FieldInfo>(*this, ReflectionKind::Field, query);
}
Result<std::optional<FieldInfo>> TypeInfo::FindField(std::string_view name,
                                                     MemberQuery query) const {
  return FindMember<FieldInfo>(*this, ReflectionKind::Field, name, query);
}
Result<std::vector<PropertyInfo>> TypeInfo::GetProperties(MemberQuery query) const {
  return GetMembers<PropertyInfo>(*this, ReflectionKind::Property, query);
}
Result<std::optional<PropertyInfo>> TypeInfo::FindProperty(std::string_view name,
                                                           MemberQuery query) const {
  return FindMember<PropertyInfo>(*this, ReflectionKind::Property, name, query);
}
Result<std::vector<MethodInfo>> TypeInfo::GetMethods(MemberQuery query) const {
  return GetMembers<MethodInfo>(*this, ReflectionKind::Method, query);
}
Result<std::optional<MethodInfo>> TypeInfo::FindMethod(std::string_view name,
                                                       MemberQuery query,
                                                       std::span<const TypeInfo> arguments) const {
  return FindMember<MethodInfo>(*this, ReflectionKind::Method, name, query, arguments);
}
Result<std::optional<ConstructorInfo>> TypeInfo::GetConstructor() const {
  return FindMember<ConstructorInfo>(*this, ReflectionKind::Constructor,
                                     reflection::kConstructorName, reflection::kConstructorQuery);
}
Result<std::vector<EnumMemberInfo>> TypeInfo::GetEnumMembers() const {
  return GetMembers<EnumMemberInfo>(*this, ReflectionKind::EnumMember, {});
}
Result<std::optional<EnumMemberInfo>> TypeInfo::FindEnumMember(std::string_view name) const {
  return FindMember<EnumMemberInfo>(*this, ReflectionKind::EnumMember, name, {});
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
Result<TypeInfo> MemberInfo::GetType() const {
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

Result<Local<Value>> MemberInfo::Read(HandleScope& scope, Local<Value> receiver) const {
  if (&receiver.isolate() != &owner_.isolate()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  return Execute(scope, owner_, [&](auto& execution) {
    return runtime::ReadReflectedMember(execution, declaration_,
                                        internal::HandleAccess::Read(receiver));
  });
}
Result<void> MemberInfo::Write(HandleScope& scope,
                               Local<Value> receiver,
                               Local<Value> value) const {
  const auto& owner = owner_.isolate();
  if (&receiver.isolate() != &owner || &value.isolate() != &owner) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  return Execute(scope, owner_,
                 [&](auto& execution) {
                   return runtime::WriteReflectedMember(execution, declaration_,
                                                        internal::HandleAccess::Read(receiver),
                                                        internal::HandleAccess::Read(value))
                       .transform([] { return runtime::Value::Null(); });
                 })
      .transform([](auto) {});
}
Result<Local<Value>> MemberInfo::Call(HandleScope& scope,
                                      Local<Value> receiver,
                                      CallArguments arguments) const {
  const auto& owner = owner_.isolate();
  if (&receiver.isolate() != &owner) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
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
    return runtime::InvokeReflectedMember(execution, declaration_,
                                          internal::HandleAccess::Read(receiver), values, names);
  });
}

Result<Local<Value>> FieldInfo::GetValue(HandleScope& scope, Local<Value> receiver) const {
  return Read(scope, receiver);
}
Result<void> FieldInfo::SetValue(HandleScope& scope,
                                 Local<Value> receiver,
                                 Local<Value> value) const {
  return Write(scope, receiver, value);
}
Result<Local<Value>> PropertyInfo::GetValue(HandleScope& scope, Local<Value> receiver) const {
  return Read(scope, receiver);
}
Result<void> PropertyInfo::SetValue(HandleScope& scope,
                                    Local<Value> receiver,
                                    Local<Value> value) const {
  return Write(scope, receiver, value);
}
Result<bool> PropertyInfo::CanRead() const {
  return CanAccess(*this, ReflectionKind::Getter);
}
Result<bool> PropertyInfo::CanWrite() const {
  return CanAccess(*this, ReflectionKind::Setter);
}
Result<std::optional<MethodInfo>> PropertyInfo::GetGetter() const {
  return GetAccessor(*this, ReflectionKind::Getter);
}
Result<std::optional<MethodInfo>> PropertyInfo::GetSetter() const {
  return GetAccessor(*this, ReflectionKind::Setter);
}
Result<std::vector<ParameterInfo>> MethodInfo::GetParameters() const {
  return Parameters();
}
Result<TypeInfo> MethodInfo::GetReturnType() const {
  return GetType();
}
Result<std::vector<TypeInfo>> MethodInfo::GetTypeArguments() const {
  return WithMember(*this, [&](auto&, const auto& index, const auto& member) {
    std::vector<runtime::ReflectedType> types;
    types.reserve(member.type_arguments.size);
    for (const auto type : member.type_arguments.values()) {
      types.push_back(index.TypeAt(type));
    }
    return BindTypes(ReflectionAccess::Binding(ReflectionAccess::Owner(*this)), types);
  });
}
Result<bool> MethodInfo::IsAsync() const {
  return WithMember(*this, [](auto&, const auto&, const auto& member) -> Result<bool> {
    return (member.flags & reflection::kAsync) != 0;
  });
}
Result<Local<Value>> MethodInfo::Invoke(HandleScope& scope,
                                        Local<Value> receiver,
                                        CallArguments arguments) const {
  return Call(scope, receiver, arguments);
}
Result<std::vector<ParameterInfo>> ConstructorInfo::GetParameters() const {
  return Parameters();
}
Result<Local<Value>> ConstructorInfo::Invoke(HandleScope& scope, CallArguments arguments) const {
  if (&scope.isolate() != &ReflectionAccess::Owner(*this).isolate()) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  const auto receiver = internal::HandleAccess::Root<Value>(scope, runtime::Value::Null());
  if (!receiver) {
    return std::unexpected(receiver.error());
  }
  return Call(scope, *receiver, arguments);
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
Result<TypeInfo> Reflection::TypeOf(HandleScope& scope, Local<Value> value) const {
  const auto& owner = binding_.isolate();
  if (&scope.isolate() != &owner || &value.isolate() != &owner) {
    return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
  }
  return WithProgram(binding_, [&](auto&) {
    return ReflectionAccess::Bind(
        binding_, {.constraint = runtime::Heap::TypeOf(internal::HandleAccess::Read(value))});
  });
}
Result<std::vector<TypeInfo>> Reflection::GetTypes(std::string_view module) const {
  return WithProgram(binding_, [&](auto& instance) -> Result<std::vector<TypeInfo>> {
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
Result<std::optional<TypeInfo>> Reflection::FindType(std::string_view module,
                                                     std::string_view name,
                                                     std::span<const TypeInfo> arguments) const {
  const auto types = ArgumentTypes(binding_, arguments);
  if (!types) {
    return std::unexpected(types.error());
  }
  return WithProgram(binding_, [&](auto& instance) -> Result<std::optional<TypeInfo>> {
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
    return std::optional<TypeInfo>(std::move(*type));
  });
}
Result<std::uint64_t> Reflection::Revision() const {
  return WithProgram(binding_, [](auto& instance) -> Result<std::uint64_t> {
    return instance.current_revision();
  });
}

}  // namespace vela
