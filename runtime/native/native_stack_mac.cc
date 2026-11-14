#include "runtime/native/native_stack.h"

#include <pthread.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "base/execution_status.h"

namespace vela::runtime {

std::expected<NativeStackBounds, ExecutionStatus> CurrentNativeStackBounds() noexcept {
  // pthread 的栈区在线程存活期间固定；只缓存成功查询，后续入口复用同一保护边界。
  constinit thread_local NativeStackBounds bounds{};
  if (bounds.upper != 0) {
    return bounds;
  }
  auto* const thread = pthread_self();
  const auto upper = std::bit_cast<std::uintptr_t>(pthread_get_stackaddr_np(thread));
  const auto size = pthread_get_stacksize_np(thread);
  if (upper == 0 || size == 0) {
    return std::unexpected(ExecutionStatus::NativeFailure);
  }
  bounds = {.lower = upper - size, .upper = upper};
  return bounds;
}

}  // namespace vela::runtime
