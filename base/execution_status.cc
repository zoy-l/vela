#include "base/execution_status.h"

#include <string_view>
#include <utility>

namespace vela {

std::string_view ExecutionStatusName(ExecutionStatus status) noexcept {
  switch (status) {
    case ExecutionStatus::Returned:
      return "returned";
    case ExecutionStatus::OutOfMemory:
      return "out of memory";
    case ExecutionStatus::TypeError:
      return "runtime type error";
    case ExecutionStatus::InvalidField:
      return "invalid field";
    case ExecutionStatus::IndexOutOfBounds:
      return "index or slice out of bounds";
    case ExecutionStatus::IntegerOverflow:
      return "integer overflow";
    case ExecutionStatus::DivisionByZero:
      return "division by zero";
    case ExecutionStatus::InvalidNumericConversion:
      return "numeric conversion is outside the target range";
    case ExecutionStatus::CyclicInitialization:
      return "cyclic global initialization";
    case ExecutionStatus::InvalidCast:
      return "checked cast failed";
    case ExecutionStatus::InactiveExpectedAlternative:
      return "accessed the inactive Expected alternative";
    case ExecutionStatus::StackOverflow:
      return "stack overflow";
    case ExecutionStatus::InvalidEntry:
      return "invalid entry function";
    case ExecutionStatus::InvalidIsolate:
      return "program belongs to a different isolate";
    case ExecutionStatus::NativeFailure:
      return "native function failed";
    case ExecutionStatus::Thrown:
      return "unhandled exception";
    case ExecutionStatus::DebuggerStopped:
      return "debugger stopped execution";
  }
  std::unreachable();
}

}  // namespace vela
