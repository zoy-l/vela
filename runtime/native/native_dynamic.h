#pragma once

#include <expected>
#include <span>
#include <string_view>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/native/native_abi.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

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
