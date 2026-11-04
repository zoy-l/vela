#pragma once

#include <cstddef>
#include <expected>

#include "base/execution_status.h"

namespace vela::runtime {

class Heap;

// 只拥有宿主内存的堆预算；C++ 存储由对象所有者释放。预算先于构造保留，失败自动退还。
class HostMemory final {
 public:
  HostMemory() = default;
  HostMemory(Heap& heap, std::size_t bytes) noexcept;
  ~HostMemory();
  HostMemory(const HostMemory&) = delete;
  HostMemory& operator=(const HostMemory&) = delete;
  HostMemory(HostMemory&& other) noexcept;
  HostMemory& operator=(HostMemory&& other) noexcept;

  [[nodiscard]] bool IsReserved() const noexcept { return heap_ != nullptr; }
  [[nodiscard]] bool IsOwnedBy(const Heap& heap) const noexcept { return heap_ == &heap; }
  [[nodiscard]] std::expected<void, ExecutionStatus> Resize(std::size_t bytes) noexcept;

 private:
  void Release() noexcept;
  Heap* heap_ = nullptr;
  std::size_t bytes_ = 0;
};

}  // namespace vela::runtime
