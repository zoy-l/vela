#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "runtime/value.h"

namespace vela::runtime {

class Isolate;

class ClassLayout final {
 public:
  [[nodiscard]] auto field_count() const noexcept -> std::uint32_t { return field_count_; }
  [[nodiscard]] auto reference_fields() const noexcept -> std::span<const std::uint32_t> {
    return reference_fields_;
  }

 private:
  friend class Isolate;

  ClassLayout(std::uint32_t field_count, std::vector<std::uint32_t> reference_fields);

  std::uint32_t field_count_;
  std::vector<std::uint32_t> reference_fields_;
};

enum class ObjectKind : std::uint8_t {
  Instance,
  Array,
};

// layout 由 Isolate 持有，生命周期覆盖所有数组与编译结果。
struct ArrayLayout final {
  ValueKind element_kind = ValueKind::Integer;
  ClassLayout* element_class = nullptr;
  const ArrayLayout* element_array = nullptr;

  [[nodiscard]] auto element_size() const noexcept -> std::size_t;
};

struct HeapObject final {
 private:
  friend class Heap;

  ObjectKind kind = ObjectKind::Instance;
  std::uint8_t mark_bit = 0;
  std::uint8_t color = 0;
  std::uint32_t allocation_size = 0;
  ClassLayout* layout = nullptr;
  HeapObject* next = nullptr;
  HeapObject* gray_next = nullptr;
  const ArrayLayout* array_layout = nullptr;
  std::uint32_t array_length = 0;
};

static_assert(alignof(HeapObject) >= alignof(Value));

// 实例字段紧接对象头，二者在同一次分配中保持连续存储。
static_assert(sizeof(HeapObject) % alignof(Value) == 0);
[[nodiscard]] auto ObjectFields(HeapObject* object) noexcept -> Value*;
[[nodiscard]] auto ObjectFields(const HeapObject* object) noexcept -> const Value*;

}  // namespace vela::runtime
