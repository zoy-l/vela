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

#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/native_function.h"
#include "runtime/object.h"
#include "runtime/random.h"

namespace vela::runtime {

struct LayoutCounts final {
  std::size_t classes = 0;
  std::size_t arrays = 0;
  std::size_t collections = 0;
  bool operator==(const LayoutCounts&) const = default;
};

class Isolate final {
 public:
  explicit Isolate(std::size_t gc_threshold = kDefaultGcThreshold) noexcept;
  ~Isolate();

  Isolate(Isolate&&) = delete;
  Isolate& operator=(Isolate&&) = delete;
  Isolate(const Isolate&) = delete;
  Isolate& operator=(const Isolate&) = delete;

  [[nodiscard]] Heap& heap() noexcept { return heap_; }
  [[nodiscard]] const Heap& heap() const noexcept { return heap_; }
  [[nodiscard]] ClassLayout& error_layout() noexcept { return *error_layout_; }
  [[nodiscard]] const ClassLayout& error_layout() const noexcept { return *error_layout_; }
  [[nodiscard]] ClassLayout& date_parts_layout() noexcept { return *date_parts_layout_; }
  [[nodiscard]] const ClassLayout& date_parts_layout() const noexcept {
    return *date_parts_layout_;
  }
  [[nodiscard]] Random& random() noexcept { return random_; }
  [[nodiscard]] FutureScheduler& scheduler() noexcept { return scheduler_; }
  [[nodiscard]] std::ostream& output() noexcept;
  void SetOutput(std::ostream& output) noexcept;

  [[nodiscard]] ClassLayout* CreateClassLayout(std::uint32_t field_count,
                                               std::span<const std::uint32_t> reference_fields,
                                               ClassLayoutOptions options = {});
  [[nodiscard]] const ArrayLayout* CreateArrayLayout(ArrayLayout layout);
  [[nodiscard]] const CollectionLayout* CreateCollectionLayout(CollectionLayout layout);
  [[nodiscard]] std::expected<void, NativeRegistrationError> RegisterNative(
      NativeFunction function);
  [[nodiscard]] std::shared_ptr<const NativeFunction> FindNative(std::string_view name) const;
  [[nodiscard]] LayoutCounts layout_counts() const noexcept;

 private:
  friend class LayoutTransaction;
  std::vector<std::unique_ptr<ClassLayout>> class_layouts_;
  std::vector<std::unique_ptr<ArrayLayout>> array_layouts_;
  std::vector<std::unique_ptr<CollectionLayout>> collection_layouts_;
  absl::flat_hash_map<std::string, std::shared_ptr<const NativeFunction>> natives_;
  ClassLayout* error_layout_ = nullptr;
  ClassLayout* date_parts_layout_ = nullptr;
  Random random_;
  FutureScheduler scheduler_;
  std::ostream* output_ = nullptr;
  Heap heap_;
};

}  // namespace vela::runtime
