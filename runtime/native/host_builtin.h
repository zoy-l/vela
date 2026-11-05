#pragma once

#include <expected>
#include <span>
#include <utility>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "base/builtin.h"
#include "runtime/native/host_value.h"
#include "runtime/value/value.h"

namespace vela::internal {

[[nodiscard]] Result<void> ExecuteHostBuiltin(HandleScope& scope,
                                              Builtin builtin,
                                              std::span<const runtime::Value> arguments,
                                              runtime::Value& output);

template <class R, class... Args>
Result<HostValue<R>> CallHostBuiltin(HandleScope& scope,
                                     Builtin builtin,
                                     const HostValue<Args>&... arguments) {
  auto values = EncodeHostArguments<Args...>(scope, arguments...);
  if (!values) {
    return std::unexpected(std::move(values.error()));
  }
  return ReceiveHostValue<R>(scope, [&](runtime::Value& output) {
    return ExecuteHostBuiltin(scope, builtin, *values, output);
  });
}

}  // namespace vela::internal
