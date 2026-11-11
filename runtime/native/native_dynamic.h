#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "base/builtin.h"
#include "base/declaration.h"
#include "base/execution_status.h"
#include "runtime/native/native_abi.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

// 调用方已验证成员和接收者并保根输入；slot 用于封闭布局，declaration 用于可重载布局。
[[nodiscard]] std::expected<Value, ExecutionStatus> ReadNativeField(NativeExecution& execution,
                                                                    Value receiver,
                                                                    std::uint32_t field,
                                                                    DeclarationId declaration,
                                                                    const TypeConstraint& type);
[[nodiscard]] std::expected<Value, ExecutionStatus> WriteNativeField(NativeExecution& execution,
                                                                     Value receiver,
                                                                     std::uint32_t field,
                                                                     DeclarationId declaration,
                                                                     Value original,
                                                                     const TypeConstraint& type);

[[nodiscard]] std::expected<Value, ExecutionStatus> ReadNativeMember(
    NativeExecution& execution,
    Value receiver,
    std::string_view name,
    const TypeConstraint& contract);
[[nodiscard]] std::expected<void, ExecutionStatus> WriteNativeMember(
    NativeExecution& execution,
    Value receiver,
    std::string_view name,
    Value value,
    const TypeConstraint& contract);

[[nodiscard]] std::expected<Value, ExecutionStatus> ExecuteNativeDynamicCall(
    NativeExecution& execution,
    Builtin builtin,
    std::span<const Value> arguments);

}  // namespace vela::runtime
