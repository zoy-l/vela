#include "runtime/native/native_stack.h"

#include <windows.h>  // IWYU pragma: keep

#include <basetsd.h>
#include <processthreadsapi.h>

#include <cstdint>
#include <expected>

#include "base/execution_status.h"

namespace vela::runtime {

std::expected<NativeStackBounds, ExecutionStatus> CurrentNativeStackBounds() noexcept {
  ULONG_PTR lower = 0;
  ULONG_PTR upper = 0;
  GetCurrentThreadStackLimits(&lower, &upper);
  return NativeStackBounds{
      .lower = static_cast<std::uintptr_t>(lower),
      .upper = static_cast<std::uintptr_t>(upper),
  };
}

}  // namespace vela::runtime
