#pragma once

#include <expected>
#include <string_view>
#include <utility>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "base/execution_status.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_type.h"
#include "runtime/native/host_value.h"
#include "runtime/native/native_type_traits.h"
#include "runtime/value/value.h"

namespace vela {
template <class T>
class TypeInfo;
namespace internal {
struct TypeAccess;
}

template <class T>
class Type final {
 public:
  static_assert(internal::kValueLogicalType<T>, "Type requires a Vela value type");

  [[nodiscard]] static Result<Type> Bind(runtime::Isolate& isolate) {
    return FromBinding(runtime::HostType::Bind(isolate, runtime::internal::NativeTypeOf<T>()));
  }
  [[nodiscard]] static Result<Type> Bind(runtime::NativeInstance& instance) {
    return FromBinding(runtime::HostType::Bind(instance, runtime::internal::NativeTypeOf<T>()));
  }
  [[nodiscard]] static Result<Type> Bind(runtime::NativeDispatch& session) {
    return FromBinding(runtime::HostType::Bind(session, runtime::internal::NativeTypeOf<T>()));
  }
  [[nodiscard]] runtime::Isolate& isolate() const noexcept { return binding_.isolate(); }
  [[nodiscard]] Result<TypeInfo<T>> Describe() const;
  [[nodiscard]] Result<HostValue<T>> Cast(HandleScope& scope, Local<Value> value) const {
    const auto& owner = isolate();
    if (&scope.isolate() != &owner || &value.isolate() != &owner) {
      return std::unexpected(Error::FromStatus(ExecutionStatus::InvalidIsolate));
    }
    return internal::ReceiveHostValue<T>(scope, [&](runtime::Value& output) {
      return internal::CastHostValue(scope, internal::HandleAccess::Read(value),
                                     binding_.constraint(), output);
    });
  }
  template <class R>
  [[nodiscard]] Result<HostValue<R>> Get(HandleScope& scope,
                                         HostValue<T> receiver,
                                         std::string_view name) const;
  template <class R>
  [[nodiscard]] Result<void> Set(HandleScope& scope,
                                 HostValue<T> receiver,
                                 std::string_view name,
                                 HostValue<R> value) const;
  template <class... Args>
  [[nodiscard]] Result<HostValue<T>> Construct(HandleScope& scope,
                                               NamedArgument<Args>... arguments) const;
  [[nodiscard]] Result<HostValue<T>> Member(HandleScope& scope, std::string_view name) const;

 private:
  friend struct internal::TypeAccess;
  explicit Type(runtime::HostType binding) noexcept : binding_(std::move(binding)) {}
  [[nodiscard]] static Result<Type> FromBinding(
      std::expected<runtime::HostType, ExecutionStatus> binding) {
    if (!binding) {
      return std::unexpected(Error::FromStatus(binding.error()));
    }
    return Type(std::move(*binding));
  }
  runtime::HostType binding_;
};

namespace internal {

struct TypeAccess final {
  template <class T>
  [[nodiscard]] static Result<Type<T>> FromBinding(
      std::expected<runtime::HostType, ExecutionStatus> binding) {
    return Type<T>::FromBinding(std::move(binding));
  }

  template <class T>
  [[nodiscard]] static const runtime::HostType& Read(const Type<T>& type) noexcept {
    return type.binding_;
  }
};

}  // namespace internal
}  // namespace vela
