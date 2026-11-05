#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <vela/type.h>

#include "runtime/native/host_object.h"
#include "runtime/native/host_value.h"
#include "runtime/native/native_type_traits.h"
#include "runtime/value/value.h"
#include "vela/error.h"
#include "vela/handles.h"
#include "vela/types.h"

namespace vela {

template <class T>
template <class... Args>
Result<HostValue<T>> Type<T>::Construct(HandleScope& scope,
                                        NamedArgument<Args>... arguments) const {
  std::array<internal::HostArgument, sizeof...(Args)> values{};
  std::size_t index = 0;
  std::optional<Error> failure;
  const auto encode = [&]<class U>(const NamedArgument<U>& argument) {
    if (!argument.value || failure) {
      return;
    }
    auto value = internal::EncodeHostValue<U>(scope, *argument.value);
    if (!value) {
      failure = std::move(value.error());
    } else {
      values[index++] = {.name = argument.name, .value = *value};
    }
  };
  (encode(arguments), ...);
  if (failure) {
    return std::unexpected(std::move(*failure));
  }
  return internal::ReceiveHostValue<T>(scope, [&](runtime::Value& output) {
    return internal::ConstructHostObject(scope, binding_, std::span(values).first(index), output);
  });
}

template <class T>
Result<HostValue<T>> Type<T>::Member(HandleScope& scope, std::string_view name) const {
  return internal::ReceiveHostValue<T>(scope, [&](runtime::Value& output) {
    return internal::ReadHostEnum(scope, binding_, name, output);
  });
}

template <class T>
template <class R>
Result<HostValue<R>> Type<T>::Get(HandleScope& scope,
                                  HostValue<T> receiver,
                                  std::string_view name) const {
  static_assert(internal::kValueLogicalType<R>, "member reads require a Vela value type");
  auto encoded = internal::EncodeHostValue<T>(scope, receiver);
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }
  return internal::ReceiveHostValue<R>(scope, [&](runtime::Value& output) {
    return internal::ReadHostMember(scope, binding_, *encoded, name,
                                    runtime::internal::NativeTypeOf<R>(), output);
  });
}

template <class T>
template <class R>
Result<void> Type<T>::Set(HandleScope& scope,
                          HostValue<T> receiver,
                          std::string_view name,
                          HostValue<R> value) const {
  auto encoded = internal::EncodeHostArguments<T, R>(scope, receiver, value);
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }
  return internal::WriteHostMember(scope, binding_, (*encoded)[0], name, (*encoded)[1]);
}

}  // namespace vela
