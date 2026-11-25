#include "runtime/heap/block_page_memory.h"

#include <windows.h>  // IWYU pragma: keep

#include <memoryapi.h>
#include <winnt.h>
#include <cstddef>

namespace vela::runtime::internal {

void* ReserveHeapBlockPage() noexcept {
  // Windows 的保留地址按 64 KiB allocation granularity 对齐。
  return VirtualAlloc(nullptr, kHeapBlockPageBytes, MEM_RESERVE, PAGE_NOACCESS);
}

HeapPageCommit CommitHeapBlockPage(void* memory, std::size_t bytes) noexcept {
  return VirtualAlloc(memory, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr
             ? HeapPageCommit::Committed
             : HeapPageCommit::OutOfMemory;
}

void DecommitHeapBlockPage(void* memory, std::size_t bytes) noexcept {
  VirtualFree(memory, bytes, MEM_DECOMMIT);
}

void ReleaseHeapBlockPage(void* memory) noexcept {
  VirtualFree(memory, 0, MEM_RELEASE);
}

}  // namespace vela::runtime::internal
