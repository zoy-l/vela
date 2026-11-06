#pragma once

#include <expected>
#include <string>
#include <vector>

#include <vela/types.h>

#include "base/execution_status.h"
#include "runtime/async/execution_context.h"
#include "runtime/async/future.h"
#include "runtime/heap/host_root.h"

namespace vela {

class HandleScope;
template <class T>
class Local;
struct Error;

template <class T>
using Result = std::expected<T, Error>;

namespace internal {
struct HandleAccess;
}

class ErrorPayload final {
 public:
  [[nodiscard]] bool IsEmpty() const noexcept { return root_.IsEmpty(); }
  [[nodiscard]] Result<Local<Value>> ToLocal(HandleScope& scope) const noexcept;

 private:
  friend struct internal::HandleAccess;
  runtime::HostRoot root_;
};

struct Error final {
  [[nodiscard]] static Error FromStatus(ExecutionStatus status) noexcept {
    return {.status = status, .message = {}, .stack_trace = {}};
  }

  ExecutionStatus status = ExecutionStatus::NativeFailure;
  std::string message;
  std::vector<runtime::FutureTraceFrame> stack_trace;
  runtime::ErrorOrigin origin = {};
  ErrorPayload payload = {};
};

}  // namespace vela
