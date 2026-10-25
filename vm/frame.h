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
  auto operator=(const Frame&) -> Frame& = delete;
  Frame(Frame&&) = delete;
  auto operator=(Frame&&) -> Frame& = delete;

  void Attach(runtime::Heap& heap, runtime::RegisterView registers) noexcept {
    registers_ = registers;
    roots_.Attach(heap, registers);
  }

  void Detach() noexcept {
    roots_.Detach();
    registers_ = {};
  }

  [[nodiscard]] auto registers() const noexcept -> runtime::RegisterView { return registers_; }

 private:
  runtime::RegisterView registers_;
  runtime::RootRange roots_;
};

}  // namespace vela::vm
