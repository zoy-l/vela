#pragma once

#include <span>

#include "runtime/heap.h"
#include "runtime/value.h"

namespace vela::vm {

class Frame final {
 public:
  Frame() noexcept = default;
  Frame(runtime::Heap& heap, std::span<runtime::Value> registers) noexcept : registers_(registers) {
    roots_.Attach(heap, registers.data(), registers.size());
  }

  ~Frame() = default;
  Frame(const Frame&) = delete;
  auto operator=(const Frame&) -> Frame& = delete;
  Frame(Frame&&) = delete;
  auto operator=(Frame&&) -> Frame& = delete;

  void Attach(runtime::Heap& heap, std::span<runtime::Value> registers) noexcept {
    registers_ = registers;
    roots_.Attach(heap, registers.data(), registers.size());
  }

  void Detach() noexcept {
    roots_.Detach();
    registers_ = {};
  }

  [[nodiscard]] auto registers() noexcept -> std::span<runtime::Value> { return registers_; }

 private:
  std::span<runtime::Value> registers_;
  runtime::RootRange roots_;
};

}  // namespace vela::vm
