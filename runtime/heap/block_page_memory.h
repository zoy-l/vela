#pragma once

#include <cstddef>
#include <cstdint>

namespace vela::runtime::internal {

// 块页向自己的大小对齐。Windows 保留地址的粒度为 64 KiB；使用完整保留区，
// 避免小块页的起始地址总落在相同的页内索引上，并减少密集遍历的虚拟地址空洞。
// Darwin arm64 使用系统的 16 KiB 页。
#ifdef _WIN32
inline constexpr std::size_t kHeapBlockPageBytes = std::size_t{64} * 1024U;
inline constexpr std::size_t kHeapBlockCommitGranularity = std::size_t{4} * 1024U;
#else
inline constexpr std::size_t kHeapBlockPageBytes = std::size_t{16} * 1024U;
inline constexpr std::size_t kHeapBlockCommitGranularity = kHeapBlockPageBytes;
#endif

enum class HeapPageCommit : std::uint8_t { Committed, OutOfMemory };

// 保留区尚不可访问；调用者按提交粒度开放实际需要的前缀，提交后才能构造页头和槽。
[[nodiscard]] void* ReserveHeapBlockPage() noexcept;
[[nodiscard]] HeapPageCommit CommitHeapBlockPage(void* memory, std::size_t bytes) noexcept;
// 调用者保证范围按提交粒度对齐，且不再包含活跃槽；保留地址随后仍可重新提交。
void DecommitHeapBlockPage(void* memory, std::size_t bytes) noexcept;
void ReleaseHeapBlockPage(void* memory) noexcept;

}  // namespace vela::runtime::internal
