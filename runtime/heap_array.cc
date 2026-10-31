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

#include "runtime/array_storage.h"
#include "runtime/element_storage.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

std::span<std::byte> Storage(HeapObject& array, const ArrayLayout& layout, std::size_t length) {
  return internal::ArrayBytes(array, layout).first(layout.element_size() * length);
}

}  // namespace

std::expected<const ArrayLayout*, ArrayAccessError> Heap::ElementLayout(
    const HeapObject& array,
    std::uint32_t index) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  if (index >= array.length) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  return array.array_layout();
}

std::expected<const ArrayLayout*, ArrayAccessError> Heap::ValueLayout(const HeapObject& array,
                                                                      Value value) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  const auto* layout = array.array_layout();
  if (!Heap::IsValueOfType(value, layout->element_type())) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  return layout;
}

HeapObject* Heap::AllocateArray(const ArrayLayout& layout, std::size_t length) noexcept {
  return AllocateArrayStorage(layout, length, {});
}

HeapObject* Heap::AllocateArrayStorage(const ArrayLayout& layout,
                                       std::size_t length,
                                       std::span<const std::byte> contents) noexcept {
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
  if (!CanAllocate(external_bytes)) {
    return nullptr;
  }
  auto* memory = external_bytes == 0
                     ? nullptr
                     : static_cast<std::byte*>(::operator new(external_bytes, std::nothrow));
  if (external_bytes != 0 && memory == nullptr) {
    return nullptr;
  }
  allocated_bytes_ += external_bytes;
  committed_bytes_ += external_bytes;
  const auto allocation = AllocateRaw(header_bytes);
  if (allocation.memory == nullptr) {
    ReleaseExternalStorage(memory, external_bytes);
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::Array;
  object->SetTypeMetadata(&layout);
  object->length = static_cast<std::uint32_t>(length);
  auto& buffer = *std::construct_at(&internal::ArrayBuffer(*object));
  buffer.capacity = inline_capacity == 0 ? object->length : inline_capacity;
  buffer.data = inline_capacity == 0 ? memory : internal::InlineArrayData(buffer);
  const auto storage = Storage(*object, layout, length);
  if (contents.empty()) {
    VisitElementStorage(layout, storage, [](auto elements) {
      std::uninitialized_value_construct_n(elements.data(), elements.size());
    });
  } else {
    std::memcpy(storage.data(), contents.data(), contents.size());
  }
  Publish(*object, allocation);
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
  if (array.kind != ObjectKind::Array || array.array_layout()->element.kind != ValueKind::Integer ||
      array.array_layout()->HasTaggedElements()) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  return ElementSlots<std::int64_t>(Storage(array, *array.array_layout(), array.length));
}

std::expected<Value, ArrayAccessError> Heap::GetElement(const HeapObject& array,
                                                        std::uint32_t index) noexcept {
  const auto checked = ElementLayout(array, index);
  if (!checked) {
    return std::unexpected(checked.error());
  }
  const auto& layout = **checked;
  return LoadElementStorage(layout, internal::ArrayBytes(array, layout), index);
}

std::expected<void, ArrayAccessError> Heap::SetElement(HeapObject& array,
                                                       std::uint32_t index,
                                                       Value value) noexcept {
  const auto checked = ElementLayout(array, index);
  if (!checked) {
    return std::unexpected(checked.error());
  }
  const auto& layout = **checked;
  if (!IsValueOfType(value, layout.element_type())) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  StoreElementStorage(layout, internal::ArrayBytes(array, layout), index, value);
  if (value.IsReference()) {
    WriteBarrier(array, value);
  }
  return {};
}

std::expected<void, ArrayAccessError> Heap::ReserveArray(HeapObject& array,
                                                         std::size_t capacity) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  return ReserveArrayStorage(array, capacity);
}

std::expected<void, ArrayAccessError> Heap::ReserveArrayStorage(HeapObject& array,
                                                                std::size_t capacity) noexcept {
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
  RootRange roots(*this, &root, 1, RootOwner::Runtime);
  const auto bytes = capacity * element_size;
  MaybeCollect(bytes);
  const auto previous_bytes =
      buffer.data == internal::InlineArrayData(buffer) ? 0 : buffer.capacity * element_size;
  if (!CanAllocate(bytes - previous_bytes)) {
    return std::unexpected(ArrayAccessError::OutOfMemory);
  }
  auto* memory = static_cast<std::byte*>(::operator new(bytes, std::nothrow));
  if (memory == nullptr) {
    return std::unexpected(ArrayAccessError::OutOfMemory);
  }
  if (array.length != 0) {
    std::memcpy(memory, buffer.data, array.length * element_size);
  }
  if (buffer.data != internal::InlineArrayData(buffer)) {
    ReleaseExternalStorage(buffer.data, previous_bytes);
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
  RootRange pending(*this, &value, 1, RootOwner::Runtime);
  return ReserveArrayStorage(array, std::max(next, static_cast<std::size_t>(array.length) + 1));
}

std::expected<std::uint32_t, ArrayAccessError> Heap::PushArray(HeapObject& array,
                                                               Value value) noexcept {
  const auto checked = ValueLayout(array, value);
  if (!checked) {
    return std::unexpected(checked.error());
  }
  const auto& layout = **checked;
  const auto grown = GrowArray(array, value);
  if (!grown) {
    return std::unexpected(grown.error());
  }
  StoreElementStorage(layout, internal::ArrayBytes(array, layout), array.length, value);
  ++array.length;
  ++internal::ArrayBuffer(array).mutation;
  if (value.IsReference()) {
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
  return RemoveArrayStorage(array, array.length - 1);
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
  RootRange roots(*this, values.data(), values.size(), RootOwner::Runtime);
  const auto capacity = internal::ArrayBuffer(array).capacity;
  if (next_length > capacity) {
    const auto next_capacity = std::max<std::size_t>(
        next_length, std::min<std::size_t>(kMaximum, static_cast<std::size_t>(capacity) * 2));
    if (auto grown = ReserveArrayStorage(array, next_capacity); !grown) {
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
  array.length = next_length;
  ++internal::ArrayBuffer(array).mutation;
  WriteBarrier(array, layout,
               storage.subspan(static_cast<std::size_t>(start) * element_size,
                               static_cast<std::size_t>(inserted) * element_size));
  return {};
}

std::expected<Value, ArrayAccessError> Heap::RemoveArrayElement(HeapObject& array,
                                                                std::uint32_t index) noexcept {
  if (const auto checked = ElementLayout(array, index); !checked) {
    return std::unexpected(checked.error());
  }
  return RemoveArrayStorage(array, index);
}

Value Heap::RemoveArrayStorage(HeapObject& array, std::uint32_t index) noexcept {
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
  MarkElements(layout, Storage(array, layout, array.length));
}

void Heap::MarkElements(const ArrayLayout& layout, std::span<const std::byte> storage) noexcept {
  VisitReferenceStorage(layout, storage, [this](auto elements) {
    for (const auto element : elements) {
      MarkReference(element);
    }
  });
}

void Heap::WriteBarrier(HeapObject& source,
                        const ArrayLayout& layout,
                        std::span<const std::byte> storage) noexcept {
  if (phase_ == GcPhase::Idle || source.mark_bit != current_mark_bit_) {
    return;
  }
  MarkElements(layout, storage);
  if (phase_ == GcPhase::Sweep) {
    DrainGray();
  }
}

void Heap::ReleaseArray(HeapObject& array) noexcept {
  auto& buffer = internal::ArrayBuffer(array);
  if (buffer.data != internal::InlineArrayData(buffer)) {
    const auto bytes = buffer.capacity * array.array_layout()->element_size();
    ReleaseExternalStorage(buffer.data, bytes);
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
  RootRange roots(*this, &source, 1, RootOwner::Runtime);
  const auto& layout = *array.array_layout();
  const auto element_size = layout.element_size();
  const auto input = Storage(array, layout, array.length)
                         .subspan(static_cast<std::size_t>(start) * element_size,
                                  static_cast<std::size_t>(end - start) * element_size);
  auto* copy = AllocateArrayStorage(layout, end - start, input);
  if (copy == nullptr) {
    return std::unexpected(ArrayAccessError::OutOfMemory);
  }
  return copy;
}

std::expected<void, ArrayAccessError> Heap::FillArray(HeapObject& array,
                                                      Value value,
                                                      std::uint32_t start,
                                                      std::uint32_t end) noexcept {
  const auto checked = ValueLayout(array, value);
  if (!checked) {
    return std::unexpected(checked.error());
  }
  const auto& layout = **checked;
  if (start > end || end > array.length) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  const auto size = layout.element_size();
  const auto storage = Storage(array, layout, array.length)
                           .subspan(static_cast<std::size_t>(start) * size,
                                    static_cast<std::size_t>(end - start) * size);
  VisitElementStorage(layout, storage, [value]<class T>(std::span<T> elements) -> void {
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
  VisitElementStorage(layout, storage,
                      [](auto elements) -> void { std::ranges::reverse(elements); });
  return {};
}

}  // namespace vela::runtime
