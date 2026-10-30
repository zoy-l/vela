#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <span>

#include "base/execution_limits.h"

namespace vela::vm {

// 只为到达的调用深度分配对象；扩展不会移动已挂接的 GC 根或调试器借用的帧。
template <class T>
class FrameSlots final {
 public:
  FrameSlots() noexcept = default;
  ~FrameSlots() = default;
  FrameSlots(const FrameSlots&) = delete;
  FrameSlots& operator=(const FrameSlots&) = delete;
  FrameSlots(FrameSlots&&) = delete;
  FrameSlots& operator=(FrameSlots&&) = delete;

  [[nodiscard]] T* Acquire(std::size_t index) noexcept {
    if (index == 0) {
      return &first_;
    }
    auto& slot = std::span(slots_).subspan(index - 1).front();
    if (slot == nullptr) {
      slot.reset(new (std::nothrow) T);
    }
    return slot.get();
  }

  [[nodiscard]] T& at(std::size_t index) noexcept {
    return index == 0 ? first_ : *std::span(slots_).subspan(index - 1).front();
  }
  [[nodiscard]] const T& at(std::size_t index) const noexcept {
    return index == 0 ? first_ : *std::span(slots_).subspan(index - 1).front();
  }

  [[nodiscard]] std::span<const std::unique_ptr<T>> additional_slots() const noexcept {
    return slots_;
  }

 private:
  // 普通单帧入口不额外分配；深层帧仍独立持有稳定地址。
  T first_;
  std::array<std::unique_ptr<T>, kMaxCallFrames - 1> slots_;
};

}  // namespace vela::vm
