#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include <vela/error.h>
#include <vela/handles.h>

#include "base/builtin.h"
#include "runtime/heap/heap.h"
#include "runtime/native/host_value.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "vela/types.h"

namespace vela::internal {

[[nodiscard]] Result<void> ReadHostArray(HandleScope& scope,
                                         runtime::Value array,
                                         std::int64_t index,
                                         runtime::Value& output);
[[nodiscard]] Result<void> WriteHostArray(HandleScope& scope,
                                          runtime::Value array,
                                          std::int64_t index,
                                          runtime::Value value);

[[nodiscard]] std::int64_t HostContainerSize(runtime::Value container) noexcept;
[[nodiscard]] std::int64_t HostArrayCapacity(runtime::Value array) noexcept;
[[nodiscard]] Result<void> CreateHostContainer(HandleScope& scope,
                                               runtime::TypeConstraint type,
                                               runtime::Value& output);
[[nodiscard]] Result<void> ExecuteHostArray(HandleScope& scope,
                                            Builtin builtin,
                                            std::span<const runtime::Value> arguments,
                                            runtime::Value& output);
[[nodiscard]] Result<HostPresence> TakeHostArray(HandleScope& scope,
                                                 Builtin builtin,
                                                 runtime::Value array,
                                                 runtime::Value& output);
[[nodiscard]] Result<void> ExecuteHostCollection(HandleScope& scope,
                                                 Builtin builtin,
                                                 std::span<const runtime::Value> arguments,
                                                 runtime::Value& output);
[[nodiscard]] Result<HostPresence> ReadHostMap(HandleScope& scope,
                                               const runtime::HeapObject& map,
                                               runtime::Value key,
                                               runtime::Value& output);
[[nodiscard]] Result<void> ProjectHostCollection(HandleScope& scope,
                                                 runtime::Value collection,
                                                 runtime::CollectionProjection projection,
                                                 runtime::Value& output);

template <class R, class Receiver, class... Args>
Result<HostValue<R>> CallHostContainer(HandleScope& scope,
                                       Builtin builtin,
                                       Local<Receiver> receiver,
                                       const HostValue<Args>&... arguments) {
  auto values = EncodeHostArguments<Receiver, Args...>(scope, receiver, arguments...);
  if (!values) {
    return std::unexpected(std::move(values.error()));
  }
  return ReceiveHostValue<R>(scope, [&](runtime::Value& output) {
    if constexpr (ArrayType<Receiver>::kIsArray) {
      return ExecuteHostArray(scope, builtin, *values, output);
    } else {
      return ExecuteHostCollection(scope, builtin, *values, output);
    }
  });
}

template <class T>
Result<std::optional<HostValue<typename ArrayType<T>::Element>>> TakeHostElement(HandleScope& scope,
                                                                                 Builtin builtin,
                                                                                 Local<T> array) {
  auto receiver = EncodeHostValue<T>(scope, array);
  if (!receiver) {
    return std::unexpected(std::move(receiver.error()));
  }
  return ReceiveOptionalHostValue<typename ArrayType<T>::Element>(
      scope,
      [&](runtime::Value& output) { return TakeHostArray(scope, builtin, *receiver, output); });
}

}  // namespace vela::internal
