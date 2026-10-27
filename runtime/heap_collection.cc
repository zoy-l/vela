#include "runtime/heap.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>

#include <absl/hash/hash.h>

#include "base/execution_status.h"
#include "runtime/element_storage.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

constexpr auto kEmpty = std::numeric_limits<std::uint32_t>::max();
constexpr auto kDeleted = kEmpty - 1;
constexpr std::uint32_t kInitialCapacity = 8;
constexpr std::uint32_t kMaximumCapacity = 1U << (std::numeric_limits<std::uint32_t>::digits - 2U);

struct CollectionStorage final {
  std::uint64_t mutation = 0;
  std::byte* data = nullptr;
  std::size_t bytes = 0;
  std::uint32_t capacity = 0;
  std::uint32_t used = 0;
  std::uint32_t size = 0;
  std::uint32_t head = kEmpty;
  std::uint32_t tail = kEmpty;
};

CollectionStorage& Storage(HeapObject& object) noexcept {
  void* data = std::span(&object, 1).last(0).data();
  return *static_cast<CollectionStorage*>(data);
}

const CollectionStorage& Storage(const HeapObject& object) noexcept {
  const void* data = std::span(&object, 1).last(0).data();
  return *static_cast<const CollectionStorage*>(data);
}

constexpr std::size_t Align(std::size_t offset, std::size_t alignment) noexcept {
  return (offset + alignment - 1) & ~(alignment - 1);
}

struct BufferLayout final {
  std::size_t key_bytes = 0;
  std::size_t value_offset = 0;
  std::size_t value_bytes = 0;
  std::size_t hashes_offset = 0;
  std::size_t buckets_offset = 0;
  std::size_t previous_offset = 0;
  std::size_t next_offset = 0;
  std::size_t bytes = 0;
};

BufferLayout LayoutFor(const CollectionLayout& type, std::size_t capacity) noexcept {
  BufferLayout layout;
  layout.key_bytes = capacity * type.keys.element_size();
  layout.value_offset = Align(layout.key_bytes, alignof(Value));
  layout.value_bytes = type.values ? capacity * type.values->element_size() : 0;
  layout.hashes_offset = Align(layout.value_offset + layout.value_bytes, alignof(std::size_t));
  layout.buckets_offset = layout.hashes_offset + (capacity * sizeof(std::size_t));
  layout.previous_offset = layout.buckets_offset + (capacity * 2 * sizeof(std::uint32_t));
  layout.next_offset = layout.previous_offset + (capacity * sizeof(std::uint32_t));
  layout.bytes = layout.next_offset + (capacity * sizeof(std::uint32_t));
  return layout;
}

struct Table final {
  std::span<std::byte> keys;
  std::span<std::byte> values;
  std::span<std::size_t> hashes;
  std::span<std::uint32_t> buckets;
  std::span<std::uint32_t> previous;
  std::span<std::uint32_t> next;
};

template <typename T>
std::span<T> Section(std::span<std::byte> bytes, std::size_t offset, std::size_t count) noexcept {
  void* data = bytes.subspan(offset, count * sizeof(T)).data();
  return {static_cast<T*>(data), count};
}

Table View(const CollectionStorage& storage, const CollectionLayout& type) noexcept {
  const auto layout = LayoutFor(type, storage.capacity);
  const auto bytes = std::span(storage.data, storage.bytes);
  const auto capacity = static_cast<std::size_t>(storage.capacity);
  return {
      .keys = bytes.first(layout.key_bytes),
      .values = bytes.subspan(layout.value_offset, layout.value_bytes),
      .hashes = Section<std::size_t>(bytes, layout.hashes_offset, capacity),
      .buckets = Section<std::uint32_t>(bytes, layout.buckets_offset, capacity * 2),
      .previous = Section<std::uint32_t>(bytes, layout.previous_offset, capacity),
      .next = Section<std::uint32_t>(bytes, layout.next_offset, capacity),
  };
}

std::size_t HashKey(Value value) noexcept {
  switch (value.kind()) {
    case ValueKind::Null:
      return absl::HashOf(value.kind());
    case ValueKind::Boolean:
      return absl::HashOf(value.kind(), value.AsBoolean());
    case ValueKind::Integer:
      return absl::HashOf(value.kind(), value.AsInteger());
    case ValueKind::Double: {
      const auto number = value.AsDouble();
      auto canonical = number;
      if (number == 0.0) {
        canonical = 0.0;
      } else if (std::isnan(number)) {
        canonical = std::numeric_limits<double>::quiet_NaN();
      }
      return absl::HashOf(value.kind(), std::bit_cast<std::uint64_t>(canonical));
    }
    case ValueKind::String:
      return absl::HashOf(value.kind(), value.AsString()->value());
    case ValueKind::Object: {
      const auto* layout = Heap::ClassOf(*value.AsObject());
      if (layout != nullptr && layout->IsEnum()) {
        return absl::HashOf(layout, Heap::GetField(*value.AsObject(), 0)->AsInteger());
      }
      return absl::HashOf(value.kind(), value.AsObject());
    }
  }
  std::unreachable();
}

bool IsSameKey(Value left, Value right) noexcept {
  if (left.kind() != right.kind()) {
    return false;
  }
  switch (left.kind()) {
    case ValueKind::Null:
      return true;
    case ValueKind::Boolean:
      return left.AsBoolean() == right.AsBoolean();
    case ValueKind::Integer:
      return left.AsInteger() == right.AsInteger();
    case ValueKind::Double: {
      const auto left_number = left.AsDouble();
      const auto right_number = right.AsDouble();
      return left_number == right_number || (std::isnan(left_number) && std::isnan(right_number));
    }
    case ValueKind::String:
      return left.AsString()->value() == right.AsString()->value();
    case ValueKind::Object: {
      const auto* layout = Heap::ClassOf(*left.AsObject());
      if (layout != nullptr && layout->IsEnum()) {
        return layout == Heap::ClassOf(*right.AsObject()) &&
               Heap::GetField(*left.AsObject(), 0)->AsInteger() ==
                   Heap::GetField(*right.AsObject(), 0)->AsInteger();
      }
      return left.AsObject() == right.AsObject();
    }
  }
  std::unreachable();
}

struct Slot final {
  std::uint32_t bucket = kEmpty;
  std::uint32_t entry = kEmpty;
  bool found = false;
};

Slot FindSlot(const Table& table,
              const CollectionLayout& layout,
              Value key,
              std::size_t hash) noexcept {
  const auto mask = static_cast<std::uint32_t>(table.buckets.size() - 1);
  auto bucket = static_cast<std::uint32_t>(hash) & mask;
  auto deleted = kEmpty;
  // 已使用项不超过 entry capacity，桶数为其两倍，因此探测必然遇到空桶。
  while (true) {
    const auto entry = table.buckets.subspan(bucket).front();
    if (entry == kEmpty) {
      return {.bucket = deleted == kEmpty ? bucket : deleted, .entry = kEmpty, .found = false};
    }
    if (entry == kDeleted) {
      if (deleted == kEmpty) {
        deleted = bucket;
      }
    } else if (table.hashes.subspan(entry).front() == hash &&
               IsSameKey(LoadElementStorage(layout.keys, table.keys, entry), key)) {
      return {.bucket = bucket, .entry = entry, .found = true};
    }
    bucket = (bucket + 1) & mask;
  }
}

void LinkTail(CollectionStorage& storage, const Table& table, std::uint32_t entry) noexcept {
  table.previous.subspan(entry).front() = storage.tail;
  table.next.subspan(entry).front() = kEmpty;
  if (storage.tail != kEmpty) {
    table.next.subspan(storage.tail).front() = entry;
  } else {
    storage.head = entry;
  }
  storage.tail = entry;
}

}  // namespace

HeapObject* Heap::AllocateCollection(const CollectionLayout& layout) noexcept {
  const auto size = sizeof(HeapObject) + sizeof(CollectionStorage);
  MaybeCollect(size);
  const auto allocation = AllocateRaw(size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::Collection;
  object->indexed_layout = &layout;
  void* data = std::span(object, 1).last(0).data();
  std::construct_at(static_cast<CollectionStorage*>(data));
  Publish(*object, allocation.size);
  return object;
}

std::expected<void, ExecutionStatus> Heap::GrowCollection(HeapObject& object) noexcept {
  auto& current = Storage(object);
  const auto& layout = *object.collection_layout();
  auto capacity = current.capacity;
  if (capacity == 0) {
    capacity = kInitialCapacity;
  } else if (current.size >= capacity / 2) {
    if (capacity >= kMaximumCapacity) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    capacity *= 2;
  }
  const auto bytes = LayoutFor(layout, capacity).bytes;
  MaybeCollect(bytes);
  auto* memory = static_cast<std::byte*>(::operator new(bytes, std::nothrow));
  if (memory == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  CollectionStorage replacement{
      .mutation = current.mutation,
      .data = memory,
      .bytes = bytes,
      .capacity = capacity,
  };
  const auto target = View(replacement, layout);
  std::ranges::fill(target.buckets, kEmpty);
  if (current.size != 0) {
    const auto source = View(current, layout);
    for (auto entry = current.head; entry != kEmpty; entry = source.next.subspan(entry).front()) {
      const auto next = replacement.used++;
      StoreElementStorage(layout.keys, target.keys, next,
                          LoadElementStorage(layout.keys, source.keys, entry));
      if (layout.values) {
        StoreElementStorage(*layout.values, target.values, next,
                            LoadElementStorage(*layout.values, source.values, entry));
      }
      const auto hash = source.hashes.subspan(entry).front();
      target.hashes.subspan(next).front() = hash;
      const auto mask = static_cast<std::uint32_t>(target.buckets.size() - 1);
      auto bucket = static_cast<std::uint32_t>(hash) & mask;
      while (target.buckets.subspan(bucket).front() != kEmpty) {
        bucket = (bucket + 1) & mask;
      }
      target.buckets.subspan(bucket).front() = next;
      LinkTail(replacement, target, next);
      ++replacement.size;
    }
  }
  allocated_bytes_ += bytes - current.bytes;
  committed_bytes_ += bytes - current.bytes;
  ::operator delete(current.data);
  current = replacement;
  return {};
}

std::expected<CollectionInsert, ExecutionStatus> Heap::InsertCollection(HeapObject& object,
                                                                        Value key,
                                                                        Value value) noexcept {
  if (object.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& layout = *object.collection_layout();
  if (!IsValueOfType(key, layout.keys.element_type()) ||
      (layout.values && !IsValueOfType(value, layout.values->element_type()))) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& storage = Storage(object);
  const auto hash = HashKey(key);
  if (storage.capacity != 0) {
    const auto table = View(storage, layout);
    const auto slot = FindSlot(table, layout, key, hash);
    if (slot.found) {
      if (layout.values) {
        StoreElementStorage(*layout.values, table.values, slot.entry, value);
        if (value.IsObject() || value.IsString()) {
          WriteBarrier(object, value);
        }
      }
      return CollectionInsert::Updated;
    }
  }
  if (storage.used == storage.capacity) {
    const auto grown = GrowCollection(object);
    if (!grown) {
      return std::unexpected(grown.error());
    }
  }
  const auto table = View(storage, layout);
  const auto slot = FindSlot(table, layout, key, hash);
  const auto entry = storage.used++;
  StoreElementStorage(layout.keys, table.keys, entry, key);
  if (layout.values) {
    StoreElementStorage(*layout.values, table.values, entry, value);
  }
  table.hashes.subspan(entry).front() = hash;
  table.buckets.subspan(slot.bucket).front() = entry;
  LinkTail(storage, table, entry);
  ++storage.size;
  ++storage.mutation;
  if (key.IsObject() || key.IsString()) {
    WriteBarrier(object, key);
  }
  if (layout.values && (value.IsObject() || value.IsString())) {
    WriteBarrier(object, value);
  }
  return CollectionInsert::Inserted;
}

std::expected<bool, ExecutionStatus> Heap::HasCollectionKey(const HeapObject& object,
                                                            Value key) noexcept {
  if (object.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& layout = *object.collection_layout();
  if (!IsValueOfType(key, layout.keys.element_type())) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& storage = Storage(object);
  return storage.size != 0 && FindSlot(View(storage, layout), layout, key, HashKey(key)).found;
}

std::expected<Value, ExecutionStatus> Heap::GetCollectionValue(const HeapObject& object,
                                                               Value key) noexcept {
  if (object.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& layout = *object.collection_layout();
  if (!layout.values || !IsValueOfType(key, layout.keys.element_type())) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& storage = Storage(object);
  if (storage.size == 0) {
    return Value::Null();
  }
  const auto table = View(storage, layout);
  const auto slot = FindSlot(table, layout, key, HashKey(key));
  return slot.found ? LoadElementStorage(*layout.values, table.values, slot.entry) : Value::Null();
}

std::expected<CollectionRemove, ExecutionStatus> Heap::RemoveCollection(HeapObject& object,
                                                                        Value key) noexcept {
  if (object.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& layout = *object.collection_layout();
  if (!IsValueOfType(key, layout.keys.element_type())) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& storage = Storage(object);
  if (storage.size == 0) {
    return CollectionRemove::Missing;
  }
  const auto table = View(storage, layout);
  const auto slot = FindSlot(table, layout, key, HashKey(key));
  if (!slot.found) {
    return CollectionRemove::Missing;
  }
  const auto previous = table.previous.subspan(slot.entry).front();
  const auto next = table.next.subspan(slot.entry).front();
  if (previous == kEmpty) {
    storage.head = next;
  } else {
    table.next.subspan(previous).front() = next;
  }
  if (next == kEmpty) {
    storage.tail = previous;
  } else {
    table.previous.subspan(next).front() = previous;
  }
  table.buckets.subspan(slot.bucket).front() = kDeleted;
  --storage.size;
  ++storage.mutation;
  if (storage.size == 0) {
    storage.used = 0;
    std::ranges::fill(table.buckets, kEmpty);
  }
  return CollectionRemove::Removed;
}

std::expected<void, ExecutionStatus> Heap::ClearCollection(HeapObject& object) noexcept {
  if (object.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& storage = Storage(object);
  if (storage.size != 0) {
    ++storage.mutation;
  }
  if (storage.capacity != 0) {
    std::ranges::fill(View(storage, *object.collection_layout()).buckets, kEmpty);
  }
  storage.used = 0;
  storage.size = 0;
  storage.head = kEmpty;
  storage.tail = kEmpty;
  return {};
}

std::expected<HeapObject*, ExecutionStatus> Heap::CombineSets(const HeapObject& left,
                                                              const HeapObject& right,
                                                              SetOperation operation) noexcept {
  if (left.kind != ObjectKind::Collection || right.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& layout = *left.collection_layout();
  if (layout.values || left.collection_layout() != right.collection_layout()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto* result = AllocateCollection(layout);
  if (result == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto rooted = Value::Object(result);
  RootRange root(*this, &rooted, 1);
  const auto append = [&](const HeapObject& input) -> std::expected<void, ExecutionStatus> {
    const auto& storage = Storage(input);
    if (storage.size == 0) {
      return {};
    }
    const auto table = View(storage, layout);
    for (auto entry = storage.head; entry != kEmpty; entry = table.next.subspan(entry).front()) {
      const auto key = LoadElementStorage(layout.keys, table.keys, entry);
      if (operation != SetOperation::Union) {
        const auto found = HasCollectionKey(right, key).value();
        if (found != (operation == SetOperation::Intersection)) {
          continue;
        }
      }
      const auto inserted = InsertCollection(*result, key);
      if (!inserted) {
        return std::unexpected(inserted.error());
      }
    }
    return {};
  };
  const auto first = append(left);
  if (!first) {
    return std::unexpected(first.error());
  }
  if (operation == SetOperation::Union) {
    const auto second = append(right);
    if (!second) {
      return std::unexpected(second.error());
    }
  }
  return result;
}

std::expected<std::uint32_t, ExecutionStatus> Heap::CollectionLength(
    const HeapObject& object) noexcept {
  if (object.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return Storage(object).size;
}

std::expected<CollectionCursor, ExecutionStatus> Heap::IterateCollection(
    const HeapObject& object) noexcept {
  if (object.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& storage = Storage(object);
  return CollectionCursor{.mutation = storage.mutation, .next = storage.head};
}

std::expected<void, ExecutionStatus> Heap::CheckCollectionMutation(
    const HeapObject& object,
    const CollectionCursor& cursor) noexcept {
  if (object.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (Storage(object).mutation != cursor.mutation) {
    return std::unexpected(ExecutionStatus::ConcurrentModification);
  }
  return {};
}

std::expected<std::optional<CollectionEntry>, ExecutionStatus> Heap::NextCollectionEntry(
    const HeapObject& object,
    CollectionCursor& cursor) noexcept {
  if (const auto checked = CheckCollectionMutation(object, cursor); !checked) {
    return std::unexpected(checked.error());
  }
  if (cursor.next == kEmpty) {
    return std::nullopt;
  }
  const auto& layout = *object.collection_layout();
  const auto table = View(Storage(object), layout);
  const auto index = cursor.next;
  cursor.next = table.next.subspan(index).front();
  return CollectionEntry{
      .key = LoadElementStorage(layout.keys, table.keys, index),
      .value =
          layout.values ? LoadElementStorage(*layout.values, table.values, index) : Value::Null(),
  };
}

std::expected<void, ExecutionStatus> Heap::CopyCollection(
    const HeapObject& object,
    HeapObject& array,
    CollectionProjection projection) noexcept {
  if (object.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& layout = *object.collection_layout();
  const auto& storage = Storage(object);
  const auto length = ArrayLength(array);
  if (!length || *length != storage.size) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* element_layout = &layout.keys;
  if (projection == CollectionProjection::Values) {
    if (!layout.values) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    element_layout = &*layout.values;
  }
  if (storage.size == 0) {
    return {};
  }
  const auto table = View(storage, layout);
  const auto elements = projection == CollectionProjection::Keys ? table.keys : table.values;
  std::uint32_t index = 0;
  for (auto entry = storage.head; entry != kEmpty; entry = table.next.subspan(entry).front()) {
    const auto value = LoadElementStorage(*element_layout, elements, entry);
    if (!SetElement(array, index++, value)) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  return {};
}

void Heap::MarkCollection(HeapObject& object) noexcept {
  const auto& layout = *object.collection_layout();
  const auto references = [](const ArrayLayout& type) -> bool {
    return type.element.dynamic || type.element.kind == ValueKind::Object ||
           type.element.kind == ValueKind::String;
  };
  const bool key_references = references(layout.keys);
  const bool value_references = layout.values && references(*layout.values);
  const auto& storage = Storage(object);
  if (storage.size == 0 || (!key_references && !value_references)) {
    return;
  }
  const auto table = View(storage, layout);
  for (auto entry = storage.head; entry != kEmpty; entry = table.next.subspan(entry).front()) {
    if (key_references) {
      MarkValue(LoadElementStorage(layout.keys, table.keys, entry));
    }
    if (value_references) {
      MarkValue(LoadElementStorage(*layout.values, table.values, entry));
    }
  }
}

void Heap::ReleaseCollection(HeapObject& object) noexcept {
  auto& storage = Storage(object);
  allocated_bytes_ -= storage.bytes;
  committed_bytes_ -= storage.bytes;
  ::operator delete(storage.data);
  std::destroy_at(&storage);
}

}  // namespace vela::runtime
