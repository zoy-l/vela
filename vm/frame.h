#pragma once

#include <cstddef>
#include <span>

#include "runtime/heap.h"

namespace vela::vm {

class Frame final {
 public:
  Frame(runtime::Heap& heap, std::span<runtime::Value> registers) noexcept
      : registers_(registers), roots_(heap, registers.data(), registers.size()) {}

  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;
  Frame(Frame&&) = delete;
  Frame& operator=(Frame&&) = delete;

  [[nodiscard]] runtime::Value* data() noexcept { return registers_.data(); }
  [[nodiscard]] const runtime::Value* data() const noexcept { return registers_.data(); }
  [[nodiscard]] std::size_t register_count() const noexcept { return registers_.size(); }

 private:
  std::span<runtime::Value> registers_;
  runtime::RootRange roots_;
};

}  // namespace vela::vm
