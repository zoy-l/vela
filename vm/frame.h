#pragma once

#include "runtime/heap.h"
#include "runtime/register_storage.h"

namespace vela::vm {

class Frame final {
 public:
  Frame() noexcept = default;

  ~Frame() = default;
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;
  Frame(Frame&&) = delete;
  Frame& operator=(Frame&&) = delete;

  void Attach(runtime::Heap& heap, runtime::RegisterView registers) noexcept {
    registers_ = registers;
    roots_.Attach(heap, registers);
  }

  // 原生直接调用借用调用者已保根的窗口，不为同一组参数重复挂接根。
  void Borrow(runtime::RegisterView registers) noexcept { registers_ = registers; }

  void Detach() noexcept {
    roots_.Detach();
    registers_ = {};
  }

  [[nodiscard]] runtime::RegisterView registers() const noexcept { return registers_; }

 private:
  runtime::RegisterView registers_;
  runtime::RootRange roots_{runtime::RootOwner::Runtime};
};

}  // namespace vela::vm
