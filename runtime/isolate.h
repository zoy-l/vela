#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "runtime/heap.h"

namespace vela::runtime {

class Isolate final {
 public:
  explicit Isolate(std::size_t gc_threshold = 64 * 1024) noexcept;
  ~Isolate() = default;

  Isolate(const Isolate&) = delete;
  Isolate& operator=(const Isolate&) = delete;

  [[nodiscard]] Heap& heap() noexcept { return heap_; }
  [[nodiscard]] const Heap& heap() const noexcept { return heap_; }

  [[nodiscard]] ClassLayout* CreateClassLayout(std::uint32_t field_count,
                                               std::span<const std::uint32_t> reference_fields);

 private:
  std::vector<std::unique_ptr<ClassLayout>> class_layouts_;
  Heap heap_;
};

}  // namespace vela::runtime
