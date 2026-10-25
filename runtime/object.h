#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "base/callable.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;

struct ClassLayoutOptions final {
  std::optional<CallableTarget> callable;
  std::vector<std::uint32_t> interfaces;
  std::vector<std::uint32_t> dispatch_targets;
};

class ClassLayout final {
 public:
  ClassLayout(const ClassLayout&) = delete;
  auto operator=(const ClassLayout&) -> ClassLayout& = delete;
  ClassLayout(ClassLayout&&) = delete;
  auto operator=(ClassLayout&&) -> ClassLayout& = delete;
  ~ClassLayout() = default;

  [[nodiscard]] auto field_count() const noexcept -> std::uint32_t { return field_count_; }
  [[nodiscard]] auto callable() const noexcept -> const std::optional<CallableTarget>& {
    return callable_;
  }
  [[nodiscard]] auto interfaces() const noexcept -> std::span<const std::uint32_t> {
    return interfaces_;
  }
  [[nodiscard]] auto dispatch_targets() const noexcept -> std::span<const std::uint32_t> {
    return dispatch_targets_;
  }
  [[nodiscard]] auto IsImplementing(std::uint32_t interface) const noexcept -> bool;
  [[nodiscard]] auto reference_fields() const noexcept -> std::span<const std::uint32_t> {
    return reference_fields_;
  }

 private:
  friend class Isolate;

  ClassLayout(std::uint32_t field_count, std::vector<std::uint32_t> reference_fields);

  std::uint32_t field_count_;
  std::vector<std::uint32_t> reference_fields_;
  std::optional<CallableTarget> callable_;
  std::vector<std::uint32_t> interfaces_;
  std::vector<std::uint32_t> dispatch_targets_;
};

enum class ObjectKind : std::uint8_t {
  Instance,
  Array,
  String,
  StringBuilder,
  Expected,
  Collection,
  Exception,
};

struct CollectionLayout;
struct TypeConstraint;

// layout 由 Isolate 持有，生命周期覆盖所有数组与编译结果。
struct ArrayLayout final {
  ValueKind element_kind = ValueKind::Integer;
  ClassLayout* element_class = nullptr;
  const ArrayLayout* element_array = nullptr;
  ObjectKind element_object_kind = ObjectKind::Instance;
  std::optional<std::uint32_t> element_interface = std::nullopt;
  const CollectionLayout* element_collection = nullptr;
  bool callable_elements = false;
  // 可空基本类型使用 Value 槽位；引用元素仍以原生指针保存空值。
  bool nullable_elements = false;

  [[nodiscard]] auto HasTaggedElements() const noexcept -> bool {
    return nullable_elements && element_kind != ValueKind::Object &&
           element_kind != ValueKind::String;
  }

  [[nodiscard]] auto element_size() const noexcept -> std::size_t;
  [[nodiscard]] auto element_type() const noexcept -> TypeConstraint;
};

struct CollectionLayout final {
  ArrayLayout keys;
  std::optional<ArrayLayout> values = std::nullopt;
};

struct HeapObject final {
 private:
  friend class Heap;
  friend class StringObject;

  ObjectKind kind = ObjectKind::Instance;
  std::uint8_t mark_bit = 0;
  std::uint32_t allocation_size = 0;
  ClassLayout* layout = nullptr;
  HeapObject* next = nullptr;
  HeapObject* gray_next = nullptr;
  // kind 已区分数组与集合；单一布局指针避免为互斥布局增加对象头开销。
  const void* indexed_layout = nullptr;
  [[nodiscard]] auto array_layout() const noexcept -> const ArrayLayout* {
    return static_cast<const ArrayLayout*>(indexed_layout);
  }
  [[nodiscard]] auto collection_layout() const noexcept -> const CollectionLayout* {
    return static_cast<const CollectionLayout*>(indexed_layout);
  }
  std::uint32_t length = 0;
};

struct TypeConstraint final {
  ValueKind kind = ValueKind::Null;
  ObjectKind object_kind = ObjectKind::Instance;
  const ClassLayout* instance = nullptr;
  const ArrayLayout* array = nullptr;
  const CollectionLayout* collection = nullptr;
  std::optional<std::uint32_t> interface = std::nullopt;
  bool nullable = false;
  bool callable = false;
};

static_assert(alignof(HeapObject) >= alignof(Value));

// 实例字段紧接对象头，二者在同一次分配中保持连续存储。
static_assert(sizeof(HeapObject) % alignof(Value) == 0);
[[nodiscard]] inline auto ObjectFields(HeapObject* object) noexcept -> Value* {
  void* storage = std::span(object, 1).last(0).data();
  return static_cast<Value*>(storage);
}
[[nodiscard]] inline auto ObjectFields(const HeapObject* object) noexcept -> const Value* {
  const void* storage = std::span(object, 1).last(0).data();
  return static_cast<const Value*>(storage);
}

}  // namespace vela::runtime
