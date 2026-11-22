#include "runtime/heap/block_page_memory.h"

#include <sys/mman.h>

#include <bit>
#include <cstddef>
#include <cstdint>

namespace vela::runtime::internal {

void* AllocateHeapBlockPage() noexcept {
  // mmap 只承诺系统页对齐；预留额外地址空间后立即取消两端映射，
  // 不触碰或提交被裁去的区域，也不经过可能保留历史碎片的 malloc zone。
  constexpr auto kReservationBytes = kHeapBlockPageBytes * 2U;
  auto* memory =
      mmap(nullptr, kReservationBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) {
    return nullptr;
  }
  const auto address = std::bit_cast<std::uintptr_t>(memory);
  const auto aligned = (address + kHeapBlockPageBytes - 1U) & ~(kHeapBlockPageBytes - 1U);
  const auto prefix = aligned - address;
  const auto suffix = kReservationBytes - prefix - kHeapBlockPageBytes;
  if (prefix != 0U) {
    munmap(memory, prefix);
  }
  if (suffix != 0U) {
    munmap(std::bit_cast<void*>(aligned + kHeapBlockPageBytes), suffix);
  }
  return std::bit_cast<void*>(aligned);
}

void ReleaseHeapBlockPage(void* memory) noexcept {
  munmap(memory, kHeapBlockPageBytes);
}

}  // namespace vela::runtime::internal
