#pragma once

#include <span>
#include <string_view>

#include <vela/error.h>
#include <vela/handles.h>

#include "runtime/native/host_type.h"
#include "runtime/native/native_type.h"
#include "runtime/value/value.h"

namespace vela::internal {

struct HostArgument final {
  std::string_view name;
  runtime::Value value;
};

[[nodiscard]] Result<void> ConstructHostObject(HandleScope& scope,
                                               const runtime::HostType& type,
                                               std::span<const HostArgument> arguments,
                                               runtime::Value& output);
[[nodiscard]] Result<void> ReadHostEnum(HandleScope& scope,
                                        const runtime::HostType& type,
                                        std::string_view name,
                                        runtime::Value& output);

[[nodiscard]] Result<void> ReadHostMember(HandleScope& scope,
                                          const runtime::HostType& type,
                                          runtime::Value receiver,
                                          std::string_view name,
                                          runtime::NativeType result,
                                          runtime::Value& output);
[[nodiscard]] Result<void> WriteHostMember(HandleScope& scope,
                                           const runtime::HostType& type,
                                           runtime::Value receiver,
                                           std::string_view name,
                                           runtime::Value value);

}  // namespace vela::internal
