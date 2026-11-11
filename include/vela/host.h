#pragma once

#include <expected>
#include <type_traits>
#include <utility>

// IWYU pragma: begin_exports
#include <vela/async.h>
#include <vela/bytes.h>
#include <vela/collections.h>
#include <vela/entry.h>
#include <vela/error.h>
#include <vela/handles.h>
#include <vela/objects.h>
#include <vela/reflection.h>
#include <vela/regexp.h>
#include <vela/strings.h>
#include <vela/type.h>
#include <vela/types.h>
// IWYU pragma: end_exports

#include "runtime/native/handle_access.h"
#include "runtime/native/host_value.h"

namespace vela {

template <class T>
Result<Local<Value>> CreateValue(HandleScope& scope, const HostValue<T>& value) {
  auto encoded = internal::EncodeHostValue<T>(scope, value);
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }
  return internal::HandleAccess::Root<Value>(scope, *encoded);
}

template <class T>
Local<Value> Local<T>::ToValue() const noexcept {
  return internal::HandleAccess::Rebind<Value>(*this);
}

template <class T>
template <class U, class... Args>
  requires std::is_same_v<U, T>
Result<HostValue<typename internal::FunctionType<U>::Return>> Local<T>::Call(
    HandleScope& scope,
    Args&&... arguments) const {
  return internal::HostFunctionCall<U>::Call(scope, *this, std::forward<Args>(arguments)...);
}

}  // namespace vela
