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

}  // namespace vela::runtime
