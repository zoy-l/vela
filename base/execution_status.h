#pragma once

#include <cstdint>
#include <string_view>

namespace vela {

enum class ExecutionStatus : std::uint8_t {
  Returned,
  OutOfMemory,
  TypeError,
  InvalidField,
  IndexOutOfBounds,
  IntegerOverflow,
  DivisionByZero,
  InvalidNumericConversion,
  CyclicInitialization,
  InvalidCast,
  InactiveExpectedAlternative,
  StackOverflow,
  InvalidEntry,
  InvalidIsolate,
  NativeFailure,
  Thrown,
  DebuggerStopped,
  ConcurrentModification,
  FutureAlreadyCompleted,
  FutureSelfAdoption,
  AsyncIdle,
};

[[nodiscard]] std::string_view ExecutionStatusName(ExecutionStatus status) noexcept;
// 稳定的程序可读标识；诊断文案由 ExecutionStatusName 独立提供。
[[nodiscard]] std::string_view ExecutionStatusCode(ExecutionStatus status) noexcept;

}  // namespace vela
