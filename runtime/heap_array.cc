#include "runtime/heap.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include "runtime/array_storage.h"
#include "runtime/element_storage.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

std::span<std::byte> Storage(HeapObject& array, const ArrayLayout& layout, std::size_t length) {
  return internal::ArrayBytes(array, layout).first(layout.element_size() * length);
}

template <class T>
std::span<T> Elements(std::span<std::byte> storage) {
  void* bytes = storage.data();
  return {static_cast<T*>(bytes), storage.size() / sizeof(T)};
}

template <class Visitor>
void VisitStorage(const ArrayLayout& layout, std::span<std::byte> storage, const Visitor& visitor) {
  if (layout.HasTaggedElements()) {
    visitor(Elements<Value>(storage));
    return;
  }
  switch (layout.element_kind) {
    case ValueKind::Boolean:
      visitor(Elements<bool>(storage));
      return;
    case ValueKind::Integer:
      visitor(Elements<std::int64_t>(storage));
      return;
    case ValueKind::Double:
      visitor(Elements<double>(storage));
      return;
    case ValueKind::String:
      visitor(Elements<StringObject*>(storage));
      return;
    case ValueKind::Object:
      visitor(Elements<HeapObject*>(storage));
      return;
    case ValueKind::Null:
      std::unreachable();
  }
  std::unreachable();
}

template <class T>
T ElementValue(Value value) {
  if constexpr (std::is_same_v<T, Value>) {
    return value;
  } else if constexpr (std::is_same_v<T, bool>) {
    return value.AsBoolean();
  } else if constexpr (std::is_same_v<T, std::int64_t>) {
    return value.AsInteger();
  } else if constexpr (std::is_same_v<T, double>) {
    return value.AsDouble();
  } else if constexpr (std::is_same_v<T, StringObject*>) {
    return value.IsNull() ? nullptr : value.AsString();
  } else {
    return value.IsNull() ? nullptr : value.AsObject();
  }
}

}  // namespace

HeapObject* Heap::AllocateArray(const ArrayLayout& layout, std::size_t length) noexcept {
  const auto element_size = layout.element_size();
  if (element_size == 0 || length > std::numeric_limits<std::uint32_t>::max() ||
      length > std::numeric_limits<std::size_t>::max() / element_size) {
    return nullptr;
  }
  const auto inline_capacity =
      length <= internal::kInlineArrayCapacity ? internal::kInlineArrayCapacity : 0;
  const auto header_bytes =
      sizeof(HeapObject) + sizeof(internal::ArrayStorage) + (inline_capacity * element_size);
  const auto external_bytes = inline_capacity == 0 ? length * element_size : 0;
  if (external_bytes > std::numeric_limits<std::size_t>::max() - header_bytes) {
    return nullptr;
  }
  MaybeCollect(header_bytes + external_bytes);
  auto* memory = external_bytes == 0
                     ? nullptr
                     : static_cast<std::byte*>(::operator new(external_bytes, std::nothrow));
  if (external_bytes != 0 && memory == nullptr) {
    return nullptr;
  }
  const auto allocation = AllocateRaw(header_bytes);
  if (allocation.memory == nullptr) {
    ::operator delete(memory);
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::Array;
  object->indexed_layout = &layout;
  object->length = static_cast<std::uint32_t>(length);
  auto& buffer = *std::construct_at(&internal::ArrayBuffer(*object));
  buffer.capacity = inline_capacity == 0 ? object->length : inline_capacity;
  buffer.data = inline_capacity == 0 ? memory : internal::InlineArrayData(buffer);
  const auto storage = Storage(*object, layout, length);
  VisitStorage(layout, storage, [](auto elements) -> void {
    std::uninitialized_value_construct_n(elements.data(), elements.size());
  });
  allocated_bytes_ += external_bytes;
  committed_bytes_ += external_bytes;
  Publish(*object, allocation.size);
  return object;
}

std::expected<std::uint32_t, ArrayAccessError> Heap::ArrayLength(const HeapObject& array) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  return array.length;
}

std::expected<std::uint32_t, ArrayAccessError> Heap::ArrayCapacity(
    const HeapObject& array) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  return internal::ArrayBuffer(array).capacity;
}

std::expected<std::uint64_t, ArrayAccessError> Heap::ArrayMutation(
    const HeapObject& array) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  return internal::ArrayBuffer(array).mutation;
}

std::expected<std::span<std::int64_t>, ArrayAccessError> Heap::IntegerElements(
    HeapObject& array) noexcept {
  if (array.kind != ObjectKind::Array || array.array_layout()->element_kind != ValueKind::Integer ||
      array.array_layout()->HasTaggedElements()) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  return Elements<std::int64_t>(Storage(array, *array.array_layout(), array.length));
}

std::expected<Value, ArrayAccessError> Heap::GetElement(const HeapObject& array,
                                                        std::uint32_t index) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  if (index >= array.length) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  const auto& layout = *array.array_layout();
  return LoadElementStorage(layout, internal::ArrayBytes(array, layout), index);
}

std::expected<void, ArrayAccessError> Heap::SetElement(HeapObject& array,
                                                       std::uint32_t index,
                                                       Value value) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  if (index >= array.length) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  const auto& layout = *array.array_layout();
  if (!IsValueOfType(value, layout.element_type())) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  StoreElementStorage(layout, internal::ArrayBytes(array, layout), index, value);
  if (value.IsObject() || value.IsString()) {
    WriteBarrier(array, value);
  }
  return {};
}

std::expected<void, ArrayAccessError> Heap::ReserveArray(HeapObject& array,
                                                         std::size_t capacity) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  auto& buffer = internal::ArrayBuffer(array);
  if (capacity <= buffer.capacity) {
    return {};
  }
  const auto element_size = array.array_layout()->element_size();
  if (capacity > std::numeric_limits<std::uint32_t>::max() ||
      capacity > std::numeric_limits<std::size_t>::max() / element_size) {
    return std::unexpected(ArrayAccessError::OutOfMemory);
  }
  const auto root = Value::Object(&array);
  RootRange roots(*this, &root, 1);
  const auto bytes = capacity * element_size;
  MaybeCollect(bytes);
  auto* memory = static_cast<std::byte*>(::operator new(bytes, std::nothrow));
  if (memory == nullptr) {
    return std::unexpected(ArrayAccessError::OutOfMemory);
  }
  if (array.length != 0) {
    std::memcpy(memory, buffer.data, array.length * element_size);
  }
  if (buffer.data != internal::InlineArrayData(buffer)) {
    const auto previous_bytes = buffer.capacity * element_size;
    allocated_bytes_ -= previous_bytes;
    committed_bytes_ -= previous_bytes;
    ::operator delete(buffer.data);
  }
  buffer.data = memory;
  buffer.capacity = static_cast<std::uint32_t>(capacity);
  allocated_bytes_ += bytes;
  committed_bytes_ += bytes;
  return {};
}

std::expected<void, ArrayAccessError> Heap::GrowArray(HeapObject& array, Value value) noexcept {
  const auto capacity = internal::ArrayBuffer(array).capacity;
  if (array.length < capacity) {
    return {};
  }
  constexpr auto kMaximum = std::numeric_limits<std::uint32_t>::max();
  if (array.length == kMaximum) {
    return std::unexpected(ArrayAccessError::OutOfMemory);
  }
  const auto next = std::min<std::size_t>(kMaximum, static_cast<std::size_t>(capacity) * 2);
  // receiver 由 ReserveArray 保根；待插入值还没有进入数组，必须单独跨 GC 保存。
  RootRange pending(*this, &value, 1);
  return ReserveArray(array, std::max(next, static_cast<std::size_t>(array.length) + 1));
}

std::expected<std::uint32_t, ArrayAccessError> Heap::PushArray(HeapObject& array,
                                                               Value value) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  const auto& layout = *array.array_layout();
  if (!IsValueOfType(value, layout.element_type())) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  const auto grown = GrowArray(array, value);
  if (!grown) {
    return std::unexpected(grown.error());
  }
  StoreElementStorage(layout, internal::ArrayBytes(array, layout), array.length, value);
  ++array.length;
  ++internal::ArrayBuffer(array).mutation;
  if (value.IsObject() || value.IsString()) {
    WriteBarrier(array, value);
  }
  return array.length;
}

std::expected<Value, ArrayAccessError> Heap::PopArray(HeapObject& array) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  if (array.length == 0) {
    return Value::Null();
  }
  return RemoveArrayElement(array, array.length - 1);
}

std::expected<void, ArrayAccessError> Heap::ReplaceArrayRange(HeapObject& array,
                                                              std::uint32_t start,
                                                              std::uint32_t count,
                                                              HeapObject& items) noexcept {
  if (array.kind != ObjectKind::Array || items.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  if (start > array.length || count > array.length - start) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  assert(&array != &items);
  const auto& layout = *array.array_layout();
  if (!IsValueOfType(
          Value::Object(&items),
          {.kind = ValueKind::Object, .object_kind = ObjectKind::Array, .array = &layout})) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  const auto length = array.length;
  const auto inserted = items.length;
  if (count == 0 && inserted == 0) {
    return {};
  }
  constexpr auto kMaximum = std::numeric_limits<std::uint32_t>::max();
  if (inserted > kMaximum - (length - count)) {
    return std::unexpected(ArrayAccessError::OutOfMemory);
  }
  const auto next_length = length - count + inserted;
  const auto values = std::to_array({Value::Object(&array), Value::Object(&items)});
  RootRange roots(*this, values.data(), values.size());
  const auto capacity = internal::ArrayBuffer(array).capacity;
  if (next_length > capacity) {
    const auto next_capacity = std::max<std::size_t>(
        next_length, std::min<std::size_t>(kMaximum, static_cast<std::size_t>(capacity) * 2));
    if (auto grown = ReserveArray(array, next_capacity); !grown) {
      return grown;
    }
  }
  const auto element_size = layout.element_size();
  const auto storage = internal::ArrayBytes(array, layout);
  const auto tail = static_cast<std::size_t>(length - start - count) * element_size;
  if (tail != 0 && count != inserted) {
    std::memmove(storage.subspan(static_cast<std::size_t>(start + inserted) * element_size).data(),
                 storage.subspan(static_cast<std::size_t>(start + count) * element_size).data(),
                 tail);
  }
  if (inserted != 0) {
    const auto input = Storage(items, layout, inserted);
    std::memcpy(storage.subspan(static_cast<std::size_t>(start) * element_size).data(),
                input.data(), input.size());
  }
  if (next_length < length) {
    std::memset(storage.subspan(static_cast<std::size_t>(next_length) * element_size).data(), 0,
                static_cast<std::size_t>(length - next_length) * element_size);
  }
  array.length = next_length;
  ++internal::ArrayBuffer(array).mutation;
  if (layout.element_kind == ValueKind::Object || layout.element_kind == ValueKind::String) {
    for (std::uint32_t index = 0; index < inserted; ++index) {
      WriteBarrier(array, LoadElementStorage(layout, storage, start + index));
    }
  }
  return {};
}

std::expected<Value, ArrayAccessError> Heap::RemoveArrayElement(HeapObject& array,
                                                                std::uint32_t index) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  if (index >= array.length) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  const auto& layout = *array.array_layout();
  const auto storage = internal::ArrayBytes(array, layout);
  const auto value = LoadElementStorage(layout, storage, index);
  const auto element_size = layout.element_size();
  const auto tail = static_cast<std::size_t>(array.length - index - 1) * element_size;
  if (tail != 0) {
    std::memmove(storage.subspan(index * element_size).data(),
                 storage.subspan((static_cast<std::size_t>(index) + 1) * element_size).data(),
                 tail);
  }
  --array.length;
  ++internal::ArrayBuffer(array).mutation;
  std::memset(storage.subspan(array.length * element_size).data(), 0, element_size);
  return value;
}

std::expected<void, ArrayAccessError> Heap::ClearArray(HeapObject& array) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  if (array.length != 0) {
    array.length = 0;
    ++internal::ArrayBuffer(array).mutation;
  }
  return {};
}

void Heap::MarkArray(HeapObject& array) noexcept {
  const auto& layout = *array.array_layout();
  const auto storage = Storage(array, layout, array.length);
  if (layout.element_kind == ValueKind::Object) {
    for (auto* child : Elements<HeapObject*>(storage)) {
      if (child != nullptr) {
        MarkObject(*child);
      }
    }
  } else if (layout.element_kind == ValueKind::String) {
    for (auto* string : Elements<StringObject*>(storage)) {
      if (string != nullptr) {
        MarkObject(string->header_);
      }
    }
  }
}

void Heap::ReleaseArray(HeapObject& array) noexcept {
  auto& buffer = internal::ArrayBuffer(array);
  if (buffer.data != internal::InlineArrayData(buffer)) {
    const auto bytes = buffer.capacity * array.array_layout()->element_size();
    allocated_bytes_ -= bytes;
    committed_bytes_ -= bytes;
    ::operator delete(buffer.data);
  }
  std::destroy_at(&buffer);
}

std::expected<HeapObject*, ArrayAccessError> Heap::SliceArray(HeapObject& array,
                                                              std::uint32_t start,
                                                              std::uint32_t end) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  if (start > end || end > array.length) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  const auto source = Value::Object(&array);
  RootRange roots(*this, &source, 1);
  const auto& layout = *array.array_layout();
  auto* copy = AllocateArray(layout, end - start);
  if (copy == nullptr) {
    return std::unexpected(ArrayAccessError::OutOfMemory);
  }
  const auto count = static_cast<std::size_t>(end - start);
  if (count != 0) {
    const auto input = Storage(array, layout, array.length)
                           .subspan(start * layout.element_size(), count * layout.element_size());
    const auto output = Storage(*copy, layout, count);
    std::memcpy(output.data(), input.data(), input.size());
    if (phase_ != GcPhase::Idle) {
      ProcessGrayObject(*copy);
    }
  }
  return copy;
}

std::expected<void, ArrayAccessError> Heap::FillArray(HeapObject& array,
                                                      Value value,
                                                      std::uint32_t start,
                                                      std::uint32_t end) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  const auto& layout = *array.array_layout();
  if (!IsValueOfType(value, layout.element_type())) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  if (start > end || end > array.length) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  const auto size = layout.element_size();
  const auto storage = Storage(array, layout, array.length)
                           .subspan(static_cast<std::size_t>(start) * size,
                                    static_cast<std::size_t>(end - start) * size);
  VisitStorage(layout, storage, [value]<class T>(std::span<T> elements) -> void {
    std::ranges::fill(elements, ElementValue<T>(value));
  });
  WriteBarrier(array, value);
  return {};
}

std::expected<void, ArrayAccessError> Heap::ReverseArray(HeapObject& array) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  const auto& layout = *array.array_layout();
  const auto storage = Storage(array, layout, array.length);
  VisitStorage(layout, storage, [](auto elements) -> void { std::ranges::reverse(elements); });
  return {};
}

}  // namespace vela::runtime
