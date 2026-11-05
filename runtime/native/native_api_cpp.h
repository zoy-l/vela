#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <type_traits>
#include <utility>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_value.h"
#include "runtime/native/native_api.h"
#include "runtime/value/value.h"

namespace vela::runtime::aot {

struct InstanceRelease final {
  void operator()(VelaAotInstance* value) const noexcept { vela_aot_v2_instance_release(value); }
};
using Instance = std::unique_ptr<VelaAotInstance, InstanceRelease>;

[[nodiscard]] Error TakeError(VelaAotStatus status, VelaAotError* error);
[[nodiscard]] Result<runtime::Value> Decode(HandleScope& scope, const VelaAotValue& input);
[[nodiscard]] VelaAotLocal Borrow(Local<vela::Value> value) noexcept;
[[nodiscard]] Result<VelaAotValue> Encode(HandleScope& scope, runtime::Value value);
[[nodiscard]] Result<Local<vela::Value>> ReceiveLocal(HandleScope& scope,
                                                      const VelaAotValue& value);

template <class T>
[[nodiscard]] Result<VelaAotValue> EncodeArgument(HandleScope& scope, const HostValue<T>& value) {
  if constexpr (requires { typename vela::internal::HostValueType<T>::Payload; }) {
    return value ? EncodeArgument<typename vela::internal::HostValueType<T>::Payload>(scope, *value)
                 : Result<VelaAotValue>(VelaAotValue{});
  } else {
    auto encoded = vela::internal::EncodeHostValue<T>(scope, value);
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    if constexpr (vela::internal::HostValueType<T>::kManaged) {
      VelaAotValue result{};
      result.kind = VELA_AOT_LOCAL;
      result.local = Borrow(vela::internal::HandleAccess::Rebind<vela::Value>(value));
      return result;
    } else {
      return Encode(scope, *encoded);
    }
  }
}

// 生成包装携带完整静态签名；参数借用已有根，C 返回的局部结果直接复用目标槽。
template <class R, class... Args, class Entry>
[[nodiscard]] Result<HostValue<R>> Invoke(HandleScope& scope,
                                          NativeInstance* instance,
                                          Entry entry,
                                          HostValue<Args>... arguments) {
  std::array<Result<VelaAotValue>, sizeof...(Args)> values{
      EncodeArgument<Args>(scope, arguments)...,
  };
  std::array<VelaAotValue, sizeof...(Args)> encoded{};
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    auto& argument = values[index];
    if (!argument) {
      return std::unexpected(std::move(argument.error()));
    }
    encoded[index] = *argument;
  }
  VelaAotValue result{};
  VelaAotError* error = nullptr;
  const auto status = entry(&scope, instance, encoded.data(), encoded.size(), &result, &error);
  if (status != VELA_AOT_RETURNED) {
    return std::unexpected(TakeError(status, error));
  }
  if constexpr (vela::internal::HostValueType<R>::kManaged) {
    auto local = ReceiveLocal(scope, result);
    if (!local) {
      return std::unexpected(std::move(local.error()));
    }
    return vela::internal::DecodeHostLocal<R>(*local);
  } else if constexpr (std::is_void_v<R>) {
    return {};
  } else {
    const auto decoded = Decode(scope, result);
    if (!decoded) {
      return std::unexpected(decoded.error());
    }
    return vela::internal::DecodeHostScalar<R>(*decoded);
  }
}

}  // namespace vela::runtime::aot
