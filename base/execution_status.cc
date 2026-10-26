#include "base/execution_status.h"

#include <string_view>
#include <utility>

namespace vela {
namespace {

struct StatusDescription final {
  std::string_view code;
  std::string_view message;
};

StatusDescription DescribeStatus(ExecutionStatus status) noexcept {
  switch (status) {
    case ExecutionStatus::Returned:
      return {.code = "Returned", .message = "returned"};
    case ExecutionStatus::OutOfMemory:
      return {.code = "OutOfMemory", .message = "out of memory"};
    case ExecutionStatus::TypeError:
      return {.code = "TypeError", .message = "runtime type error"};
    case ExecutionStatus::InvalidField:
      return {.code = "InvalidField", .message = "invalid field"};
    case ExecutionStatus::IndexOutOfBounds:
      return {.code = "IndexOutOfBounds", .message = "index or slice out of bounds"};
    case ExecutionStatus::IntegerOverflow:
      return {.code = "IntegerOverflow", .message = "integer overflow"};
    case ExecutionStatus::DivisionByZero:
      return {.code = "DivisionByZero", .message = "division by zero"};
    case ExecutionStatus::InvalidNumericConversion:
      return {
          .code = "InvalidNumericConversion",
          .message = "numeric conversion is outside the target range",
      };
    case ExecutionStatus::CyclicInitialization:
      return {.code = "CyclicInitialization", .message = "cyclic global initialization"};
    case ExecutionStatus::InvalidCast:
      return {.code = "InvalidCast", .message = "checked cast failed"};
    case ExecutionStatus::InactiveExpectedAlternative:
      return {
          .code = "InactiveExpectedAlternative",
          .message = "accessed the inactive Expected alternative",
      };
    case ExecutionStatus::StackOverflow:
      return {.code = "StackOverflow", .message = "stack overflow"};
    case ExecutionStatus::InvalidEntry:
      return {.code = "InvalidEntry", .message = "invalid entry function"};
    case ExecutionStatus::InvalidIsolate:
      return {.code = "InvalidIsolate", .message = "program belongs to a different isolate"};
    case ExecutionStatus::NativeFailure:
      return {.code = "NativeFailure", .message = "native function failed"};
    case ExecutionStatus::Thrown:
      return {.code = "Thrown", .message = "unhandled exception"};
    case ExecutionStatus::DebuggerStopped:
      return {.code = "DebuggerStopped", .message = "debugger stopped execution"};
    case ExecutionStatus::ConcurrentModification:
      return {
          .code = "ConcurrentModification",
          .message = "array structurally modified during callback",
      };
    case ExecutionStatus::FutureAlreadyCompleted:
      return {
          .code = "FutureAlreadyCompleted",
          .message = "Future completion was already requested",
      };
    case ExecutionStatus::FutureSelfAdoption:
      return {.code = "FutureSelfAdoption", .message = "Future cannot adopt itself"};
    case ExecutionStatus::AsyncIdle:
      return {.code = "AsyncIdle", .message = "async root has no pending producer"};
  }
  std::unreachable();
}

}  // namespace

std::string_view ExecutionStatusName(ExecutionStatus status) noexcept {
  return DescribeStatus(status).message;
}

std::string_view ExecutionStatusCode(ExecutionStatus status) noexcept {
  return DescribeStatus(status).code;
}

}  // namespace vela
