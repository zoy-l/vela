#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include "base/execution_status.h"
#include "runtime/future_composition.h"
#include "runtime/native_abi.h"
#include "runtime/native_instance.h"
#include "runtime/value.h"

namespace vela::runtime {

// 仅捕获 Isolate 与代码身份；回调的实例状态由 callable 的 managed 边保留。
[[nodiscard]] FutureCallbacks NativeFutureCallbacks(const NativeInstance& instance);

namespace internal {

[[nodiscard]] std::expected<Value, ExecutionStatus> BoxNativeEnumResult(NativeExecution& execution,
                                                                        std::uint32_t type,
                                                                        Value value);

}  // namespace internal

// 参数已经过调用点绑定与验证；返回 runtime 表示，enum 结果在此完成盒装。
[[nodiscard]] inline std::expected<Value, ExecutionStatus> InvokeNativeTarget(
    NativeExecution& execution,
    std::uint32_t function,
    std::span<const NativeValue> arguments) {
  const auto& target = execution.instance().image().functions.values().subspan(function).front();
  NativeValue output;
  const auto status = static_cast<ExecutionStatus>(
      target.entry(&execution, arguments.data(), arguments.size(), &output));
  if (status != ExecutionStatus::Returned) {
    return std::unexpected(status);
  }
  const auto value = DecodeNativeValue(output);
  return (target.flags & kNativeEnumResult) != 0
             ? internal::BoxNativeEnumResult(execution, target.result_type, value)
             : std::expected<Value, ExecutionStatus>(value);
}

// indirect 参数区首槽预留给闭包环境或绑定接收者；显式实参从第二槽开始。
extern "C" std::uint8_t vela_native_v1_call_indirect(NativeExecution* execution,
                                                     const NativeValue* callable,
                                                     NativeValue* arguments,
                                                     std::uint64_t count,
                                                     NativeValue* result);
extern "C" std::uint8_t vela_native_v1_call_interface(NativeExecution* execution,
                                                      std::uint32_t selector,
                                                      const NativeValue* arguments,
                                                      std::uint64_t count,
                                                      NativeValue* result);

}  // namespace vela::runtime
