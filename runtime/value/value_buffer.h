#pragma once

#include <cstddef>
#include <span>

#include "runtime/value/value.h"

namespace vela::runtime {

// 不触发 GC 的连续临时存储；系统分配失败由调用者在边界通过 IsAllocated 检查。
// values 借用本对象，只有成功分配后才允许访问；调用者另外负责托管引用的保根。
class ValueBuffer final {
 public:
  explicit ValueBuffer(std::size_t count) noexcept;
  ~ValueBuffer();
  ValueBuffer(const ValueBuffer&) = delete;
  ValueBuffer& operator=(const ValueBuffer&) = delete;
  ValueBuffer(ValueBuffer&&) = delete;
  ValueBuffer& operator=(ValueBuffer&&) = delete;

  [[nodiscard]] bool IsAllocated() const noexcept { return data_ != nullptr || count_ == 0; }
  [[nodiscard]] std::span<Value> values() noexcept { return {data_, count_}; }

 private:
  Value* data_ = nullptr;
  std::size_t count_;
};

}  // namespace vela::runtime
