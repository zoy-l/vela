#pragma once

#include <cstddef>

namespace vela::runtime::internal {

// 页地址可由块地址向下对齐得到；大小同时满足支持平台的 VM 页与保留粒度。
inline constexpr std::size_t kHeapBlockPageBytes = std::size_t{64} * 1024U;

[[nodiscard]] void* AllocateHeapBlockPage() noexcept;
void ReleaseHeapBlockPage(void* memory) noexcept;

}  // namespace vela::runtime::internal
