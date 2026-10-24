#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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
  std::unique_ptr<std::vector<Value>> array_values;
};

static_assert(alignof(HeapObject) >= alignof(Value));

// 实例字段紧接对象头，二者在同一次分配中保持连续存储。
static_assert(sizeof(HeapObject) % alignof(Value) == 0);
[[nodiscard]] auto ObjectFields(HeapObject* object) noexcept -> Value*;
[[nodiscard]] auto ObjectFields(const HeapObject* object) noexcept -> const Value*;

}  // namespace vela::runtime
