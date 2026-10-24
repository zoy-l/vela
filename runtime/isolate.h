#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "runtime/heap.h"
#include "runtime/string.h"

namespace vela::runtime {

class Isolate final {
 public:
  explicit Isolate(std::size_t gc_threshold = kDefaultGcThreshold) noexcept;
  ~Isolate() = default;

  Isolate(Isolate&&) = delete;
  auto operator=(Isolate&&) -> Isolate& = delete;
  Isolate(const Isolate&) = delete;
  auto operator=(const Isolate&) -> Isolate& = delete;

  [[nodiscard]] auto heap() noexcept -> Heap& { return heap_; }
  [[nodiscard]] auto heap() const noexcept -> const Heap& { return heap_; }

  [[nodiscard]] auto CreateClassLayout(std::uint32_t field_count,
                                       std::span<const std::uint32_t> reference_fields)
      -> ClassLayout*;
  [[nodiscard]] auto CreateString(std::string value) -> StringObject*;

 private:
  std::vector<std::unique_ptr<ClassLayout>> class_layouts_;
  std::vector<std::unique_ptr<StringObject>> strings_;
  Heap heap_;
};

}  // namespace vela::runtime
