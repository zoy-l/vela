#include "runtime/native_stack.h"

#include <pthread.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "base/execution_status.h"

namespace vela::runtime {

std::expected<NativeStackBounds, ExecutionStatus> CurrentNativeStackBounds() noexcept {
  const auto thread = pthread_self();
  const auto upper = std::bit_cast<std::uintptr_t>(pthread_get_stackaddr_np(thread));
  const auto size = pthread_get_stacksize_np(thread);
  if (upper == 0 || size == 0) {
    return std::unexpected(ExecutionStatus::NativeFailure);
  }
  return NativeStackBounds{.lower = upper - size, .upper = upper};
}

}  // namespace vela::runtime
