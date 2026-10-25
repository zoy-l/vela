#pragma once

#include <cstddef>

#include "runtime/register_storage.h"

namespace vela::vm {

class RegisterFile final {
 public:
  RegisterFile() = default;
  ~RegisterFile();
  RegisterFile(const RegisterFile&) = delete;
  auto operator=(const RegisterFile&) -> RegisterFile& = delete;
  RegisterFile(RegisterFile&&) = delete;
  auto operator=(RegisterFile&&) -> RegisterFile& = delete;
  // 仅在对应调用深度无活跃帧时获取；返回区间在本帧期间保持地址稳定。
  [[nodiscard]] auto Acquire(std::size_t count) noexcept -> runtime::RegisterView;
  [[nodiscard]] auto capacity() const noexcept -> std::size_t { return capacity_; }

 private:
  runtime::RegisterPayload* values_ = nullptr;
  std::size_t capacity_ = 0;
};

}  // namespace vela::vm
