#include "runtime/native_stack.h"

#include <pthread.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "base/execution_status.h"

namespace vela::runtime {

std::expected<NativeStackBounds, ExecutionStatus> CurrentNativeStackBounds() noexcept {
  pthread_attr_t attributes;
  if (pthread_getattr_np(pthread_self(), &attributes) != 0) {
    return std::unexpected(ExecutionStatus::NativeFailure);
  }
  void* address = nullptr;
  std::size_t size = 0;
  const auto result = pthread_attr_getstack(&attributes, &address, &size);
  pthread_attr_destroy(&attributes);
  if (result != 0) {
    return std::unexpected(ExecutionStatus::NativeFailure);
  }
  const auto lower = std::bit_cast<std::uintptr_t>(address);
  return NativeStackBounds{.lower = lower, .upper = lower + size};
}

}  // namespace vela::runtime
