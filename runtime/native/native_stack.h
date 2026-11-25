#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

#include "base/execution_status.h"

namespace vela::runtime {

// 为异常构造、宿主返回和展开保留栈空间；生成代码在每次原生入口检查同一边界。
inline constexpr std::size_t kNativeStackReserveBytes = std::size_t{64} * 1024;

struct NativeStackBounds final {
  std::uintptr_t lower;
  std::uintptr_t upper;
};

[[nodiscard]] std::expected<NativeStackBounds, ExecutionStatus> CurrentNativeStackBounds() noexcept;

#ifdef _WIN32
[[nodiscard]] bool IsOnNativeThreadStack() noexcept;
#endif

[[nodiscard]] inline std::expected<std::uintptr_t, ExecutionStatus> NativeStackLimit(
    NativeStackBounds bounds) noexcept {
  if (bounds.upper - bounds.lower <= kNativeStackReserveBytes) {
    return std::unexpected(ExecutionStatus::StackOverflow);
  }
  return bounds.lower + kNativeStackReserveBytes;
}

[[nodiscard]] inline std::expected<std::uintptr_t, ExecutionStatus>
CurrentNativeStackLimit() noexcept {
  const auto bounds = CurrentNativeStackBounds();
  if (!bounds) {
    return std::unexpected(bounds.error());
  }
  return NativeStackLimit(*bounds);
}

}  // namespace vela::runtime
