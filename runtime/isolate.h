#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "runtime/heap.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "runtime/random.h"

namespace vela::runtime {

struct LayoutCounts final {
  std::size_t classes = 0;
  std::size_t arrays = 0;
  std::size_t collections = 0;
  auto operator==(const LayoutCounts&) const -> bool = default;
};

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
  [[nodiscard]] auto error_layout() noexcept -> ClassLayout& { return *error_layout_; }
  [[nodiscard]] auto error_layout() const noexcept -> const ClassLayout& { return *error_layout_; }
  [[nodiscard]] auto random() noexcept -> Random& { return random_; }
  [[nodiscard]] auto input() noexcept -> std::istream&;
  [[nodiscard]] auto output() noexcept -> std::ostream&;
  void SetConsole(std::istream& input, std::ostream& output) noexcept;

  [[nodiscard]] auto CreateClassLayout(std::uint32_t field_count,
                                       std::span<const std::uint32_t> reference_fields,
                                       ClassLayoutOptions options = {}) -> ClassLayout*;
  [[nodiscard]] auto CreateArrayLayout(ArrayLayout layout) -> const ArrayLayout*;
  [[nodiscard]] auto CreateCollectionLayout(CollectionLayout layout) -> const CollectionLayout*;
  [[nodiscard]] auto RegisterNative(NativeFunction function)
      -> std::expected<void, NativeRegistrationError>;
  [[nodiscard]] auto FindNative(std::string_view name) const
      -> std::shared_ptr<const NativeFunction>;
  [[nodiscard]] auto layout_counts() const noexcept -> LayoutCounts;

 private:
  friend class LayoutTransaction;
  std::vector<std::unique_ptr<ClassLayout>> class_layouts_;
  std::vector<std::unique_ptr<ArrayLayout>> array_layouts_;
  std::vector<std::unique_ptr<CollectionLayout>> collection_layouts_;
  absl::flat_hash_map<std::string, std::shared_ptr<const NativeFunction>> natives_;
  ClassLayout* error_layout_ = nullptr;
  Random random_;
  std::istream* input_ = nullptr;
  std::ostream* output_ = nullptr;
  Heap heap_;
};

}  // namespace vela::runtime
