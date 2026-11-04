#include "runtime/heap/host_memory.h"

#include <cassert>
#include <cstddef>
#include <expected>
#include <utility>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"

namespace vela::runtime {

HostMemory::HostMemory(Heap& heap, std::size_t bytes) noexcept {
  if (heap.ReserveHostMemory(bytes)) {
    heap_ = &heap;
    bytes_ = bytes;
  }
}

HostMemory::~HostMemory() {
  Release();
}

HostMemory::HostMemory(HostMemory&& other) noexcept
    : heap_(std::exchange(other.heap_, nullptr)), bytes_(std::exchange(other.bytes_, 0)) {}

HostMemory& HostMemory::operator=(HostMemory&& other) noexcept {
  if (this != &other) {
    Release();
    heap_ = std::exchange(other.heap_, nullptr);
    bytes_ = std::exchange(other.bytes_, 0);
  }
  return *this;
}

void HostMemory::Release() noexcept {
  if (heap_ != nullptr) {
    heap_->ReleaseHostMemory(bytes_);
    heap_ = nullptr;
    bytes_ = 0;
  }
}

std::expected<void, ExecutionStatus> HostMemory::Resize(std::size_t bytes) noexcept {
  assert(IsReserved());
  if (bytes > bytes_) {
    if (!heap_->ReserveHostMemory(bytes - bytes_)) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
  } else {
    heap_->ReleaseHostMemory(bytes_ - bytes);
  }
  bytes_ = bytes;
  return {};
}

}  // namespace vela::runtime
