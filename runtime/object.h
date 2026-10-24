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
  [[nodiscard]] std::uint32_t field_count() const noexcept { return field_count_; }
  [[nodiscard]] std::span<const std::uint32_t> reference_fields() const noexcept {
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
};

static_assert(alignof(HeapObject) >= alignof(Value));

[[nodiscard]] inline Value* ObjectFields(HeapObject* object) noexcept {
  auto* bytes = reinterpret_cast<std::byte*>(object);
  return reinterpret_cast<Value*>(bytes + sizeof(HeapObject));
}

[[nodiscard]] inline const Value* ObjectFields(const HeapObject* object) noexcept {
  const auto* bytes = reinterpret_cast<const std::byte*>(object);
  return reinterpret_cast<const Value*>(bytes + sizeof(HeapObject));
}

}  // namespace vela::runtime
