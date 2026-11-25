#include "runtime/heap/block_page_memory.h"

#include <sys/mman.h>

#include <bit>
#include <cstddef>
#include <cstdint>

namespace vela::runtime::internal {

void* ReserveHeapBlockPage(std::size_t bytes) noexcept {
  // mmap 只承诺系统页对齐；预留额外地址空间后立即取消两端映射，
  // 不触碰或提交被裁去的区域，也不经过可能保留历史碎片的 malloc zone。
  const auto reservation_bytes = bytes * 2U;
  auto* memory = mmap(nullptr, reservation_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) {
    return nullptr;
  }
  const auto address = std::bit_cast<std::uintptr_t>(memory);
  const auto aligned = (address + bytes - 1U) & ~(bytes - 1U);
  const auto prefix = aligned - address;
  const auto suffix = reservation_bytes - prefix - bytes;
  if (prefix != 0U) {
    munmap(memory, prefix);
  }
  if (suffix != 0U) {
    munmap(std::bit_cast<void*>(aligned + bytes), suffix);
  }
  return std::bit_cast<void*>(aligned);
}

HeapPageCommit CommitHeapBlockPage(void* memory, std::size_t bytes) noexcept {
  return mprotect(memory, bytes, PROT_READ | PROT_WRITE) == 0 ? HeapPageCommit::Committed
                                                              : HeapPageCommit::OutOfMemory;
}

void DecommitHeapBlockPage(void* memory, std::size_t bytes) noexcept {
  mprotect(memory, bytes, PROT_NONE);
  madvise(memory, bytes, MADV_DONTNEED);
}

void ReleaseHeapBlockPage(void* memory, std::size_t bytes) noexcept {
  munmap(memory, bytes);
}

}  // namespace vela::runtime::internal
