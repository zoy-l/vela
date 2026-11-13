#pragma once

#include <expected>
#include <span>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/native/native_abi.h"
#include "runtime/value/value.h"

namespace vela::runtime {

// 快照借用生成调用帧；帧在整个 helper 调用期间保根所有 NativeValue。
[[nodiscard]] std::expected<Value, ExecutionStatus> ExecuteReflectedInvocation(
    NativeExecution& execution,
    std::span<const NativeValue> snapshot);

[[nodiscard]] std::expected<Value, ExecutionStatus> ExecuteNativeReflection(
    NativeExecution& execution,
    Builtin builtin,
    std::span<const Value> arguments);

}  // namespace vela::runtime
