#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/type.h>

#include "base/declaration.h"
#include "base/reflection.h"
#include "runtime/native/host_type.h"
#include "runtime/reflection/reflection_index.h"
#include "runtime/value/value.h"
#include "vela/types.h"

namespace vela {
namespace internal {
struct ReflectionAccess;
}

using reflection::TypeKind;
using ReflectionMemberKind = reflection::MemberKind;
using reflection::MemberScope;
using MemberVisibility = reflection::Visibility;
using MemberVisibilityFilter = reflection::VisibilityFilter;

struct MemberQuery final {
  MemberScope scope = MemberScope::IncludeInherited;
  MemberVisibilityFilter visibility = MemberVisibilityFilter::Public;
};

template <class T>
class TypeInfo;
template <class T>
class FieldInfo;
template <class T>
class PropertyInfo;
template <class T>
class MethodInfo;
class StaticFieldInfo;
class StaticPropertyInfo;
class StaticMethodInfo;
class ConstructorInfo;
class EnumMemberInfo;
class MemberInfo;
class TypeDescriptor;
struct TypeDescription;
struct MemberDescription;

class TypeId final {
 public:
  bool operator==(const TypeId& other) const noexcept;
  template <class H>
  friend H AbslHashValue(H hash, const TypeId& identity) {
    return H::combine(std::move(hash), &identity.binding_.isolate(), identity.binding_.constraint(),
                      identity.is_void_);
  }

 private:
  friend class TypeDescriptor;
  TypeId(runtime::HostType binding, bool is_void)
      : binding_(std::move(binding)), is_void_(is_void) {}
  runtime::HostType binding_;
  bool is_void_;
};

class MemberId final {
 public:
  bool operator==(const MemberId& other) const noexcept;
  template <class H>
  friend H AbslHashValue(H hash, const MemberId& identity) {
    return H::combine(std::move(hash), &identity.binding_.isolate(), identity.binding_.constraint(),
                      identity.binding_.context_identity(), identity.declaration_);
  }

 private:
  friend class MemberInfo;
  MemberId(runtime::HostType binding, DeclarationId declaration)
      : binding_(std::move(binding)), declaration_(declaration) {}
  runtime::HostType binding_;
  DeclarationId declaration_;
};

// 描述拥有类型根。可重载描述只保存身份，查询与执行均选择当前发布。
class TypeDescriptor {
 public:
  [[nodiscard]] runtime::Isolate& isolate() const noexcept { return binding_.isolate(); }
  [[nodiscard]] TypeId id() const { return {binding_, is_void_}; }
  [[nodiscard]] Result<TypeDescription> Describe() const;
  [[nodiscard]] Result<bool> IsInstance(HandleScope& scope, Local<Value> value) const;
  [[nodiscard]] Result<bool> IsAssignableFrom(const TypeDescriptor& other) const;
  [[nodiscard]] Result<std::vector<StaticFieldInfo>> GetStaticFields(MemberQuery query = {}) const;
  [[nodiscard]] Result<std::optional<StaticFieldInfo>> FindStaticField(
      std::string_view name,
      MemberQuery query = {}) const;
  [[nodiscard]] Result<std::vector<StaticPropertyInfo>> GetStaticProperties(
      MemberQuery query = {}) const;
  [[nodiscard]] Result<std::optional<StaticPropertyInfo>> FindStaticProperty(
      std::string_view name,
      MemberQuery query = {}) const;
  [[nodiscard]] Result<std::vector<StaticMethodInfo>> GetStaticMethods(
      MemberQuery query = {}) const;
  [[nodiscard]] Result<std::optional<StaticMethodInfo>> FindStaticMethod(
      std::string_view name,
      MemberQuery query = {},
      std::span<const TypeDescriptor> type_arguments = {}) const;
  [[nodiscard]] Result<std::optional<ConstructorInfo>> GetConstructor() const;
  [[nodiscard]] Result<std::vector<EnumMemberInfo>> GetEnumMembers() const;
  [[nodiscard]] Result<std::optional<EnumMemberInfo>> FindEnumMember(std::string_view name) const;

 protected:
  TypeDescriptor(runtime::HostType binding, bool is_void)
      : binding_(std::move(binding)), is_void_(is_void) {}
  template <class M>
  [[nodiscard]] Result<std::vector<M>> SelectMembers(ReflectionMemberKind kind,
                                                     reflection::MemberStorage storage,
                                                     MemberQuery query) const;
  template <class M>
  [[nodiscard]] Result<std::optional<M>> SelectMember(
      ReflectionMemberKind kind,
      reflection::MemberStorage storage,
      std::string_view name,
      MemberQuery query,
      std::span<const TypeDescriptor> arguments = {}) const;

 private:
  friend struct internal::ReflectionAccess;
  [[nodiscard]] Result<std::vector<MemberInfo>> Members(ReflectionMemberKind kind,
                                                        reflection::MemberStorage storage,
                                                        MemberQuery query) const;
  [[nodiscard]] Result<std::optional<MemberInfo>> FindMember(
      ReflectionMemberKind kind,
      reflection::MemberStorage storage,
      std::string_view name,
      MemberQuery query,
      std::span<const TypeDescriptor> arguments) const;
  [[nodiscard]] runtime::ReflectedType type() const noexcept {
    return {.constraint = binding_.constraint(), .is_void = is_void_};
  }
  runtime::HostType binding_;
  bool is_void_;
};

template <class T>
class TypeInfo final : public TypeDescriptor {
 public:
  static_assert(internal::kValueLogicalType<T>);
  [[nodiscard]] Result<std::vector<FieldInfo<T>>> GetFields(MemberQuery query = {}) const {
    return SelectMembers<FieldInfo<T>>(ReflectionMemberKind::Field,
                                       reflection::MemberStorage::Instance, query);
  }
  [[nodiscard]] Result<std::optional<FieldInfo<T>>> FindField(std::string_view name,
                                                              MemberQuery query = {}) const {
    return SelectMember<FieldInfo<T>>(ReflectionMemberKind::Field,
                                      reflection::MemberStorage::Instance, name, query);
  }
  [[nodiscard]] Result<std::vector<PropertyInfo<T>>> GetProperties(MemberQuery query = {}) const {
    return SelectMembers<PropertyInfo<T>>(ReflectionMemberKind::Property,
                                          reflection::MemberStorage::Instance, query);
  }
  [[nodiscard]] Result<std::optional<PropertyInfo<T>>> FindProperty(std::string_view name,
                                                                    MemberQuery query = {}) const {
    return SelectMember<PropertyInfo<T>>(ReflectionMemberKind::Property,
                                         reflection::MemberStorage::Instance, name, query);
  }
  [[nodiscard]] Result<std::vector<MethodInfo<T>>> GetMethods(MemberQuery query = {}) const {
    return SelectMembers<MethodInfo<T>>(ReflectionMemberKind::Method,
                                        reflection::MemberStorage::Instance, query);
  }
  [[nodiscard]] Result<std::optional<MethodInfo<T>>> FindMethod(
      std::string_view name,
      MemberQuery query = {},
      std::span<const TypeDescriptor> arguments = {}) const {
    return SelectMember<MethodInfo<T>>(ReflectionMemberKind::Method,
                                       reflection::MemberStorage::Instance, name, query, arguments);
  }

 private:
  friend struct internal::ReflectionAccess;
  template <class U>
  friend class Type;
  TypeInfo(runtime::HostType binding, bool is_void) : TypeDescriptor(std::move(binding), is_void) {}
};

struct SignatureParameterInfo final {
  TypeInfo<Value> type;
  bool is_optional;
  bool is_rest;
};
struct SignatureInfo final {
  std::vector<SignatureParameterInfo> parameters;
  TypeInfo<Value> return_type;
};
struct TypeDescription final {
  TypeId id;
  std::string name;
  std::optional<std::string> module;
  TypeKind kind;
  bool is_nullable;
  bool is_registered;
  std::vector<TypeInfo<Value>> type_arguments;
  std::optional<TypeInfo<Value>> base_type;
  std::vector<TypeInfo<Value>> interfaces;
  std::optional<SignatureInfo> signature;
};
struct ParameterInfo final {
  std::string name;
  std::size_t position;
  TypeInfo<Value> type;
  bool is_optional;
  bool is_rest;
  bool has_default;
};
struct CallArgument final {
  explicit CallArgument(Local<Value> argument,
                        std::optional<std::string_view> argument_name = std::nullopt)
      : value(argument), name(argument_name) {}
  Local<Value> value;
  std::optional<std::string_view> name;
};
using CallArguments = std::span<const CallArgument>;

class MemberInfo {
 public:
  [[nodiscard]] MemberId id() const;
  [[nodiscard]] Result<MemberDescription> Describe() const;
  [[nodiscard]] Result<TypeInfo<Value>> GetType() const;

 protected:
  friend struct internal::ReflectionAccess;
  MemberInfo(TypeInfo<Value> owner, DeclarationId declaration)
      : owner_(std::move(owner)), declaration_(declaration) {}
  [[nodiscard]] Result<Local<Value>> Read(HandleScope& scope, runtime::Value receiver) const;
  [[nodiscard]] Result<void> Write(HandleScope& scope,
                                   runtime::Value receiver,
                                   Local<Value> value) const;
  [[nodiscard]] Result<Local<Value>> Call(HandleScope& scope,
                                          runtime::Value receiver,
                                          CallArguments arguments) const;
  [[nodiscard]] Result<std::vector<ParameterInfo>> Parameters() const;

 private:
  TypeInfo<Value> owner_;
  DeclarationId declaration_;
};

struct MemberDescription final {
  MemberId id;
  std::string name;
  TypeInfo<Value> declaring_type;
  ReflectionMemberKind kind;
  MemberVisibility visibility;
  bool is_static;
};

class PropertyDescriptor : public MemberInfo {
 public:
  [[nodiscard]] Result<bool> CanRead() const;
  [[nodiscard]] Result<bool> CanWrite() const;

 protected:
  explicit PropertyDescriptor(MemberInfo member) : MemberInfo(std::move(member)) {}
  [[nodiscard]] Result<std::optional<MemberInfo>> Accessor(ReflectionMemberKind kind) const;
};
class MethodDescriptor : public MemberInfo {
 public:
  [[nodiscard]] Result<std::vector<ParameterInfo>> GetParameters() const;
  [[nodiscard]] Result<TypeInfo<Value>> GetReturnType() const;
  [[nodiscard]] Result<std::vector<TypeInfo<Value>>> GetTypeArguments() const;
  [[nodiscard]] Result<bool> IsAsync() const;

 protected:
  explicit MethodDescriptor(MemberInfo member) : MemberInfo(std::move(member)) {}
};

template <class T>
class FieldInfo final : public MemberInfo {
 public:
  [[nodiscard]] Result<Local<Value>> GetValue(HandleScope& scope, HostValue<T> receiver) const {
    return internal::EncodeHostValue<T>(scope, receiver).and_then([&](auto value) {
      return Read(scope, value);
    });
  }
  [[nodiscard]] Result<void> SetValue(HandleScope& scope,
                                      HostValue<T> receiver,
                                      Local<Value> value) const {
    return internal::EncodeHostValue<T>(scope, receiver).and_then([&](auto target) {
      return Write(scope, target, value);
    });
  }

 private:
  friend class TypeDescriptor;
  explicit FieldInfo(MemberInfo member) : MemberInfo(std::move(member)) {}
};
class StaticFieldInfo final : public MemberInfo {
 public:
  [[nodiscard]] Result<Local<Value>> GetValue(HandleScope& scope) const;
  [[nodiscard]] Result<void> SetValue(HandleScope& scope, Local<Value> value) const;

 private:
  friend class TypeDescriptor;
  explicit StaticFieldInfo(MemberInfo member) : MemberInfo(std::move(member)) {}
};

template <class T>
class MethodInfo final : public MethodDescriptor {
 public:
  [[nodiscard]] Result<Local<Value>> Invoke(HandleScope& scope,
                                            HostValue<T> receiver,
                                            CallArguments arguments = {}) const {
    return internal::EncodeHostValue<T>(scope, receiver).and_then([&](auto target) {
      return Call(scope, target, arguments);
    });
  }

 private:
  friend class TypeDescriptor;
  friend class PropertyInfo<T>;
  explicit MethodInfo(MemberInfo member) : MethodDescriptor(std::move(member)) {}
};
class StaticMethodInfo final : public MethodDescriptor {
 public:
  [[nodiscard]] Result<Local<Value>> Invoke(HandleScope& scope, CallArguments arguments = {}) const;

 private:
  friend class TypeDescriptor;
  friend class StaticPropertyInfo;
  explicit StaticMethodInfo(MemberInfo member) : MethodDescriptor(std::move(member)) {}
};

template <class T>
class PropertyInfo final : public PropertyDescriptor {
 public:
  [[nodiscard]] Result<std::optional<MethodInfo<T>>> GetGetter() const {
    return GetAccessor(ReflectionMemberKind::Getter);
  }
  [[nodiscard]] Result<std::optional<MethodInfo<T>>> GetSetter() const {
    return GetAccessor(ReflectionMemberKind::Setter);
  }
  [[nodiscard]] Result<Local<Value>> GetValue(HandleScope& scope, HostValue<T> receiver) const {
    return internal::EncodeHostValue<T>(scope, receiver).and_then([&](auto target) {
      return Read(scope, target);
    });
  }
  [[nodiscard]] Result<void> SetValue(HandleScope& scope,
                                      HostValue<T> receiver,
                                      Local<Value> value) const {
    return internal::EncodeHostValue<T>(scope, receiver).and_then([&](auto target) {
      return Write(scope, target, value);
    });
  }

 private:
  friend class TypeDescriptor;
  explicit PropertyInfo(MemberInfo member) : PropertyDescriptor(std::move(member)) {}
  [[nodiscard]] Result<std::optional<MethodInfo<T>>> GetAccessor(ReflectionMemberKind kind) const {
    return Accessor(kind).transform([](auto member) -> std::optional<MethodInfo<T>> {
      if (!member) {
        return std::nullopt;
      }
      return MethodInfo<T>(std::move(*member));
    });
  }
};
class StaticPropertyInfo final : public PropertyDescriptor {
 public:
  [[nodiscard]] Result<std::optional<StaticMethodInfo>> GetGetter() const;
  [[nodiscard]] Result<std::optional<StaticMethodInfo>> GetSetter() const;
  [[nodiscard]] Result<Local<Value>> GetValue(HandleScope& scope) const;
  [[nodiscard]] Result<void> SetValue(HandleScope& scope, Local<Value> value) const;

 private:
  friend class TypeDescriptor;
  explicit StaticPropertyInfo(MemberInfo member) : PropertyDescriptor(std::move(member)) {}
  [[nodiscard]] Result<std::optional<StaticMethodInfo>> GetAccessor(
      ReflectionMemberKind kind) const;
};
class ConstructorInfo final : public MemberInfo {
 public:
  [[nodiscard]] Result<std::vector<ParameterInfo>> GetParameters() const;
  [[nodiscard]] Result<Local<Value>> Invoke(HandleScope& scope, CallArguments arguments = {}) const;

 private:
  friend class TypeDescriptor;
  explicit ConstructorInfo(MemberInfo member) : MemberInfo(std::move(member)) {}
};
class EnumMemberInfo final : public MemberInfo {
 public:
  [[nodiscard]] Result<std::int64_t> Ordinal() const;
  [[nodiscard]] Result<Local<Value>> GetValue(HandleScope& scope) const;

 private:
  friend class TypeDescriptor;
  explicit EnumMemberInfo(MemberInfo member) : MemberInfo(std::move(member)) {}
};

class Reflection final {
 public:
  [[nodiscard]] static Result<Reflection> Bind(runtime::NativeInstance& instance);
  [[nodiscard]] static Result<Reflection> Bind(runtime::NativeDispatch& dispatch);
  [[nodiscard]] Result<TypeInfo<Value>> TypeOf(HandleScope& scope, Local<Value> value) const;
  [[nodiscard]] Result<std::vector<TypeInfo<Value>>> GetTypes(std::string_view module) const;
  [[nodiscard]] Result<std::optional<TypeInfo<Value>>> FindType(
      std::string_view module,
      std::string_view name,
      std::span<const TypeDescriptor> type_arguments = {}) const;
  [[nodiscard]] Result<std::uint64_t> Revision() const;

 private:
  explicit Reflection(runtime::HostType binding) : binding_(std::move(binding)) {}
  runtime::HostType binding_;
};

template <class M>
Result<std::vector<M>> TypeDescriptor::SelectMembers(ReflectionMemberKind kind,
                                                     reflection::MemberStorage storage,
                                                     MemberQuery query) const {
  return Members(kind, storage, query).transform([](auto members) {
    std::vector<M> result;
    result.reserve(members.size());
    for (auto& member : members) {
      result.push_back(M(std::move(member)));
    }
    return result;
  });
}
template <class M>
Result<std::optional<M>> TypeDescriptor::SelectMember(
    ReflectionMemberKind kind,
    reflection::MemberStorage storage,
    std::string_view name,
    MemberQuery query,
    std::span<const TypeDescriptor> arguments) const {
  return FindMember(kind, storage, name, query, arguments)
      .transform([](auto member) -> std::optional<M> {
        if (!member) {
          return std::nullopt;
        }
        return M(std::move(*member));
      });
}
template <class T>
Result<TypeInfo<T>> Type<T>::Describe() const {
  return TypeInfo<T>(binding_, false);
}

}  // namespace vela
