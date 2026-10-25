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
};

[[nodiscard]] std::string_view ExecutionStatusName(ExecutionStatus status) noexcept;

}  // namespace vela
