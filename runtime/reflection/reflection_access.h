#pragma once

#include <expected>
#include <span>
#include <string_view>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "runtime/native/native_abi.h"
#include "runtime/value/value.h"

namespace vela::runtime {

[[nodiscard]] std::expected<Value, ExecutionStatus> ReadReflectedMember(NativeExecution& execution,
                                                                        DeclarationId declaration,
                                                                        Value receiver);
[[nodiscard]] std::expected<void, ExecutionStatus> WriteReflectedMember(NativeExecution& execution,
                                                                        DeclarationId declaration,
                                                                        Value receiver,
                                                                        Value original);
// values 与 names 为同一顺序、等长的稳定快照；空名称仅表示位置实参。
[[nodiscard]] std::expected<Value, ExecutionStatus> InvokeReflectedMember(
    NativeExecution& execution,
    DeclarationId declaration,
    Value receiver,
    std::span<const Value> values,
    std::span<const std::string_view> names);
[[nodiscard]] std::expected<Value, ExecutionStatus> ReadReflectedEnumMember(
    NativeExecution& execution,
    DeclarationId declaration);

}  // namespace vela::runtime
