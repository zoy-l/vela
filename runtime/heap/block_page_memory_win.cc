#include "runtime/heap/block_page_memory.h"

#include <windows.h>  // IWYU pragma: keep

#include <memoryapi.h>
#include <winnt.h>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace vela::runtime::internal {

void* ReserveHeapBlockPage(std::size_t bytes) noexcept {
  // Windows 的保留地址按 64 KiB allocation granularity 对齐。
  const auto reservation = bytes <= kMinimumHeapBlockPageBytes ? bytes : bytes * 2U;
  auto* memory = VirtualAlloc(nullptr, reservation, MEM_RESERVE, PAGE_NOACCESS);
  if (memory == nullptr) {
    return nullptr;
  }
  const auto address = std::bit_cast<std::uintptr_t>(memory);
  return std::bit_cast<void*>((address + bytes - 1U) & ~(bytes - 1U));
}

HeapPageCommit CommitHeapBlockPage(void* memory, std::size_t bytes) noexcept {
  return VirtualAlloc(memory, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr
             ? HeapPageCommit::Committed
             : HeapPageCommit::OutOfMemory;
}

void DecommitHeapBlockPage(void* memory, std::size_t bytes) noexcept {
  VirtualFree(memory, bytes, MEM_DECOMMIT);
}

void ReleaseHeapBlockPage(void* memory, std::size_t bytes) noexcept {
  if (bytes > kMinimumHeapBlockPageBytes) {
    // 大保留区可能返回对齐后的内部地址；查询同一保留区的真实起点再整段释放。
    MEMORY_BASIC_INFORMATION information{};
    VirtualQuery(memory, &information, sizeof(information));
    memory = information.AllocationBase;
  }
  VirtualFree(memory, 0, MEM_RELEASE);
}

}  // namespace vela::runtime::internal
