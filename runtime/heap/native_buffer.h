#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>

#include "runtime/heap/heap.h"

namespace vela::runtime {

// 临时原生存储参与 Heap 核算；构造时可能触发 GC，借用的托管输入须由调用者保根。
// 元素无须析构，内容由调用者初始化；values 不能跨越本对象或 Heap 的寿命。
template <class T, std::size_t InlineCapacity = 0>
class NativeBuffer final {
 public:
  static_assert(std::is_trivially_destructible_v<T>);
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(alignof(T) <= alignof(std::max_align_t));

  NativeBuffer(Heap& heap, std::size_t count) noexcept : heap_(&heap), count_(count) {
    if (count <= InlineCapacity) {
      data_ = inline_.data();
    } else if (count <= std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      data_ = static_cast<T*>(heap.AllocateNative(count * sizeof(T)));
    }
  }
  ~NativeBuffer() {
    if (count_ > InlineCapacity) {
      heap_->FreeNative(data_);
    }
  }
  NativeBuffer(const NativeBuffer&) = delete;
  NativeBuffer& operator=(const NativeBuffer&) = delete;
  NativeBuffer(NativeBuffer&&) = delete;
  NativeBuffer& operator=(NativeBuffer&&) = delete;

  [[nodiscard]] bool IsAllocated() const noexcept { return count_ == 0 || data_ != nullptr; }
  [[nodiscard]] std::span<T> values() noexcept { return {data_, count_}; }
  [[nodiscard]] std::span<const T> values() const noexcept { return {data_, count_}; }

 private:
  std::array<T, InlineCapacity> inline_;
  Heap* heap_;
  T* data_ = nullptr;
  std::size_t count_;
};

}  // namespace vela::runtime
