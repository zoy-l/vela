#include "runtime/heap/block_page_memory.h"

#include <windows.h>  // IWYU pragma: keep

#include <memoryapi.h>
#include <winnt.h>

namespace vela::runtime::internal {

void* AllocateHeapBlockPage() noexcept {
  // Windows 的保留地址按 64 KiB allocation granularity 对齐。
  return VirtualAlloc(nullptr, kHeapBlockPageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void ReleaseHeapBlockPage(void* memory) noexcept {
  VirtualFree(memory, 0, MEM_RELEASE);
}

}  // namespace vela::runtime::internal
