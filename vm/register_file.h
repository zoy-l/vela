#pragma once

#include <cstddef>

#include "runtime/value/register_storage.h"

namespace vela::vm {

class RegisterFile final {
 public:
  RegisterFile() = default;
  ~RegisterFile();
  RegisterFile(const RegisterFile&) = delete;
  RegisterFile& operator=(const RegisterFile&) = delete;
  RegisterFile(RegisterFile&&) = delete;
  RegisterFile& operator=(RegisterFile&&) = delete;
  // 仅在对应调用深度无活跃帧时获取；返回区间在本帧期间保持地址稳定。
  [[nodiscard]] runtime::RegisterView Acquire(std::size_t count) noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  runtime::RegisterPayload* values_ = nullptr;
  std::size_t capacity_ = 0;
};

}  // namespace vela::vm
