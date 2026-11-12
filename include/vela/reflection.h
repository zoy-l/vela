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
#include "vela/types.h"

namespace vela {
namespace internal {
struct ReflectionAccess;
}

using reflection::TypeKind;
using ReflectionMemberKind = reflection::MemberKind;
using reflection::MemberScope;
using MemberStorageFilter = reflection::MemberStorage;
using MemberVisibility = reflection::Visibility;
using MemberVisibilityFilter = reflection::VisibilityFilter;
using reflection::MemberQuery;

class FieldInfo;
class PropertyInfo;
class MethodInfo;
class ConstructorInfo;
class EnumMemberInfo;
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
  friend class TypeInfo;
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
class TypeInfo final {
 public:
  [[nodiscard]] runtime::Isolate& isolate() const noexcept { return binding_.isolate(); }
  [[nodiscard]] TypeId id() const { return {binding_, is_void_}; }
  [[nodiscard]] Result<TypeDescription> Describe() const;
  [[nodiscard]] Result<bool> IsInstance(HandleScope& scope, Local<Value> value) const;
  [[nodiscard]] Result<bool> IsAssignableFrom(const TypeInfo& other) const;
  [[nodiscard]] Result<std::vector<FieldInfo>> GetFields(MemberQuery query = {}) const;
  [[nodiscard]] Result<std::optional<FieldInfo>> FindField(std::string_view name,
                                                           MemberQuery query = {}) const;
  [[nodiscard]] Result<std::vector<PropertyInfo>> GetProperties(MemberQuery query = {}) const;
  [[nodiscard]] Result<std::optional<PropertyInfo>> FindProperty(std::string_view name,
                                                                 MemberQuery query = {}) const;
  [[nodiscard]] Result<std::vector<MethodInfo>> GetMethods(MemberQuery query = {}) const;
  [[nodiscard]] Result<std::optional<MethodInfo>> FindMethod(
      std::string_view name,
      MemberQuery query = {},
      std::span<const TypeInfo> type_arguments = {}) const;
  [[nodiscard]] Result<std::optional<ConstructorInfo>> GetConstructor() const;
  [[nodiscard]] Result<std::vector<EnumMemberInfo>> GetEnumMembers() const;
  [[nodiscard]] Result<std::optional<EnumMemberInfo>> FindEnumMember(std::string_view name) const;

 private:
  friend struct internal::ReflectionAccess;
  friend class Reflection;
  template <class T>
  friend class Type;
  TypeInfo(runtime::HostType binding, bool is_void)
      : binding_(std::move(binding)), is_void_(is_void) {}
  [[nodiscard]] runtime::ReflectedType type() const noexcept {
    return {.constraint = binding_.constraint(), .is_void = is_void_};
  }
  runtime::HostType binding_;
  bool is_void_;
};

struct SignatureParameterInfo final {
  TypeInfo type;
  bool is_optional;
  bool is_rest;
};
struct SignatureInfo final {
  std::vector<SignatureParameterInfo> parameters;
  TypeInfo return_type;
};
struct TypeDescription final {
  TypeId id;
  std::string name;
  std::optional<std::string> module;
  TypeKind kind;
  bool is_nullable;
  bool is_registered;
  std::vector<TypeInfo> type_arguments;
  std::optional<TypeInfo> base_type;
  std::vector<TypeInfo> interfaces;
  std::optional<SignatureInfo> signature;
};

struct ParameterInfo final {
  std::string name;
  std::size_t position;
  TypeInfo type;
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
  [[nodiscard]] Result<TypeInfo> GetType() const;

 protected:
  friend struct internal::ReflectionAccess;
  MemberInfo(TypeInfo owner, DeclarationId declaration)
      : owner_(std::move(owner)), declaration_(declaration) {}
  [[nodiscard]] Result<Local<Value>> Read(HandleScope& scope, Local<Value> receiver) const;
  [[nodiscard]] Result<void> Write(HandleScope& scope,
                                   Local<Value> receiver,
                                   Local<Value> value) const;
  [[nodiscard]] Result<Local<Value>> Call(HandleScope& scope,
                                          Local<Value> receiver,
                                          CallArguments arguments) const;
  [[nodiscard]] Result<std::vector<ParameterInfo>> Parameters() const;

 private:
  TypeInfo owner_;
  DeclarationId declaration_;
};

struct MemberDescription final {
  MemberId id;
  std::string name;
  TypeInfo declaring_type;
  ReflectionMemberKind kind;
  MemberVisibility visibility;
  bool is_static;
};

class FieldInfo final : public MemberInfo {
 public:
  [[nodiscard]] Result<Local<Value>> GetValue(HandleScope& scope, Local<Value> receiver) const;
  [[nodiscard]] Result<void> SetValue(HandleScope& scope,
                                      Local<Value> receiver,
                                      Local<Value> value) const;

 private:
  friend struct internal::ReflectionAccess;
  using MemberInfo::MemberInfo;
};
class PropertyInfo final : public MemberInfo {
 public:
  [[nodiscard]] Result<bool> CanRead() const;
  [[nodiscard]] Result<bool> CanWrite() const;
  [[nodiscard]] Result<std::optional<MethodInfo>> GetGetter() const;
  [[nodiscard]] Result<std::optional<MethodInfo>> GetSetter() const;
  [[nodiscard]] Result<Local<Value>> GetValue(HandleScope& scope, Local<Value> receiver) const;
  [[nodiscard]] Result<void> SetValue(HandleScope& scope,
                                      Local<Value> receiver,
                                      Local<Value> value) const;

 private:
  friend struct internal::ReflectionAccess;
  using MemberInfo::MemberInfo;
};
class MethodInfo final : public MemberInfo {
 public:
  [[nodiscard]] Result<std::vector<ParameterInfo>> GetParameters() const;
  [[nodiscard]] Result<TypeInfo> GetReturnType() const;
  [[nodiscard]] Result<std::vector<TypeInfo>> GetTypeArguments() const;
  [[nodiscard]] Result<bool> IsAsync() const;
  [[nodiscard]] Result<Local<Value>> Invoke(HandleScope& scope,
                                            Local<Value> receiver,
                                            CallArguments arguments = {}) const;

 private:
  friend struct internal::ReflectionAccess;
  using MemberInfo::MemberInfo;
};
class ConstructorInfo final : public MemberInfo {
 public:
  [[nodiscard]] Result<std::vector<ParameterInfo>> GetParameters() const;
  [[nodiscard]] Result<Local<Value>> Invoke(HandleScope& scope, CallArguments arguments = {}) const;

 private:
  friend struct internal::ReflectionAccess;
  using MemberInfo::MemberInfo;
};
class EnumMemberInfo final : public MemberInfo {
 public:
  [[nodiscard]] Result<std::int64_t> Ordinal() const;
  [[nodiscard]] Result<Local<Value>> GetValue(HandleScope& scope) const;

 private:
  friend struct internal::ReflectionAccess;
  using MemberInfo::MemberInfo;
};

class Reflection final {
 public:
  [[nodiscard]] static Result<Reflection> Bind(runtime::NativeInstance& instance);
  [[nodiscard]] static Result<Reflection> Bind(runtime::NativeDispatch& dispatch);
  [[nodiscard]] Result<TypeInfo> TypeOf(HandleScope& scope, Local<Value> value) const;
  [[nodiscard]] Result<std::vector<TypeInfo>> GetTypes(std::string_view module) const;
  [[nodiscard]] Result<std::optional<TypeInfo>> FindType(
      std::string_view module,
      std::string_view name,
      std::span<const TypeInfo> type_arguments = {}) const;
  [[nodiscard]] Result<std::uint64_t> Revision() const;

 private:
  explicit Reflection(runtime::HostType binding) : binding_(std::move(binding)) {}
  runtime::HostType binding_;
};

template <class T>
Result<TypeInfo> Type<T>::Describe() const {
  return TypeInfo(binding_, false);
}

}  // namespace vela
