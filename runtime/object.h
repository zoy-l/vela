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
class Future;
class Timer;
class CancellationToken;

struct ClassLayoutOptions final {
  std::optional<CallableTarget> callable;
  std::vector<std::uint32_t> interfaces;
  std::vector<std::uint32_t> dispatch_targets;
};

class ClassLayout final {
 public:
  ClassLayout(const ClassLayout&) = delete;
  ClassLayout& operator=(const ClassLayout&) = delete;
  ClassLayout(ClassLayout&&) = delete;
  ClassLayout& operator=(ClassLayout&&) = delete;
  ~ClassLayout() = default;

  [[nodiscard]] std::uint32_t field_count() const noexcept { return field_count_; }
  [[nodiscard]] const std::optional<CallableTarget>& callable() const noexcept { return callable_; }
  [[nodiscard]] std::span<const std::uint32_t> interfaces() const noexcept { return interfaces_; }
  [[nodiscard]] std::span<const std::uint32_t> dispatch_targets() const noexcept {
    return dispatch_targets_;
  }
  [[nodiscard]] bool IsImplementing(std::uint32_t interface) const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> reference_fields() const noexcept {
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
  MutableString,
  Expected,
  Collection,
  Exception,
  Future,
  Completer,
  Timer,
  CancellationToken,
  ByteArray,
  ByteData,
  ValueSlots,
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

  [[nodiscard]] bool HasTaggedElements() const noexcept {
    return nullable_elements && element_kind != ValueKind::Object &&
           element_kind != ValueKind::String;
  }

  [[nodiscard]] std::size_t element_size() const noexcept;
  [[nodiscard]] TypeConstraint element_type() const noexcept;
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
  const ClassLayout* layout = nullptr;
  HeapObject* next = nullptr;
  HeapObject* gray_next = nullptr;
  // kind 已区分数组与集合；单一布局指针避免为互斥布局增加对象头开销。
  const void* indexed_layout = nullptr;
  [[nodiscard]] const ArrayLayout* array_layout() const noexcept {
    return static_cast<const ArrayLayout*>(indexed_layout);
  }
  [[nodiscard]] const CollectionLayout* collection_layout() const noexcept {
    return static_cast<const CollectionLayout*>(indexed_layout);
  }
  std::uint32_t length = 0;
  // kind 确定 native_payload 的类型；底层存储由 Heap 按该类型销毁。
  void* native_payload = nullptr;
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
[[nodiscard]] inline Value* ObjectFields(HeapObject* object) noexcept {
  void* storage = std::span(object, 1).last(0).data();
  return static_cast<Value*>(storage);
}
[[nodiscard]] inline const Value* ObjectFields(const HeapObject* object) noexcept {
  const void* storage = std::span(object, 1).last(0).data();
  return static_cast<const Value*>(storage);
}

}  // namespace vela::runtime
