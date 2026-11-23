#pragma once

#include <cstddef>

namespace vela::runtime::internal {

// 页地址可由块地址向下对齐得到。16 KiB 覆盖支持平台的系统页，限制非移动活对象
// 固定住的空闲容量；Windows 独立保留的基址仍由 VirtualAlloc 按 64 KiB 对齐。
inline constexpr std::size_t kHeapBlockPageBytes = std::size_t{16} * 1024U;

[[nodiscard]] void* AllocateHeapBlockPage() noexcept;
void ReleaseHeapBlockPage(void* memory) noexcept;

}  // namespace vela::runtime::internal
