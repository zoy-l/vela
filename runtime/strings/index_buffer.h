#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <span>

#include "base/execution_status.h"

namespace vela::runtime {

// 搜索前缀和 Unicode 边界的临时索引；短输入留在栈上，分配失败返回执行错误。
// values 借用本对象的存储，不得跨越其析构。
class IndexBuffer final {
 public:
  explicit IndexBuffer(std::size_t count) noexcept
      : data_(count <= inline_.size() ? inline_.data() : Allocate(count)), count_(count) {}
  ~IndexBuffer() {
    if (data_ != inline_.data()) {
      Free(data_);
    }
  }
  IndexBuffer(const IndexBuffer&) = delete;
  IndexBuffer& operator=(const IndexBuffer&) = delete;
  IndexBuffer(IndexBuffer&&) = delete;
  IndexBuffer& operator=(IndexBuffer&&) = delete;

  [[nodiscard]] std::expected<std::span<std::size_t>, ExecutionStatus> values() noexcept {
    if (data_ == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    return std::span(data_, count_);
  }

 private:
  static std::size_t* Allocate(std::size_t count) noexcept;
  static void Free(std::size_t* data) noexcept;
  static constexpr std::size_t kInlineCapacity = 64;
  std::array<std::size_t, kInlineCapacity> inline_{};
  std::size_t* data_;
  std::size_t count_;
};

}  // namespace vela::runtime
