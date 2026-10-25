#pragma once

#include "runtime/heap.h"
#include "runtime/register_storage.h"

namespace vela::vm {

class Frame final {
 public:
  Frame() noexcept = default;
  Frame(runtime::Heap& heap, runtime::RegisterView registers) noexcept : registers_(registers) {
    roots_.Attach(heap, registers);
  }

  ~Frame() = default;
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;
  Frame(Frame&&) = delete;
  Frame& operator=(Frame&&) = delete;

  void Attach(runtime::Heap& heap, runtime::RegisterView registers) noexcept {
    registers_ = registers;
    roots_.Attach(heap, registers);
  }

  void Detach() noexcept {
    roots_.Detach();
    registers_ = {};
  }

  [[nodiscard]] runtime::RegisterView registers() const noexcept { return registers_; }

 private:
  runtime::RegisterView registers_;
  runtime::RootRange roots_;
};

}  // namespace vela::vm
