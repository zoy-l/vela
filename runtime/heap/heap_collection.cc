#include "runtime/heap/heap.h"

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
#include "runtime/heap/element_storage.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

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
  return *StorageAfter<CollectionStorage>(object);
}

const CollectionStorage& Storage(const HeapObject& object) noexcept {
  return *StorageAfter<CollectionStorage>(object);
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
        return absl::HashOf(&layout->nominal_type(),
                            Heap::GetField(*value.AsObject(), 0)->AsInteger());
      }
      return absl::HashOf(value.kind(), value.AsObject());
    }
  }
  std::unreachable();
}

bool IsSameKey(Value left, Value right) noexcept {
  if (left.kind() == ValueKind::Double && right.kind() == ValueKind::Double &&
      std::isnan(left.AsDouble()) && std::isnan(right.AsDouble())) {
    return true;
  }
  return IsEqualValue(left, right);
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

struct CollectionLookup final {
  Table table;
  Slot slot;
};

std::expected<CollectionLookup, ExecutionStatus> FindEntry(const HeapObject& object,
                                                           Value key) noexcept {
  const auto* layout = Heap::CollectionTypeOf(object);
  if (layout == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (!Heap::IsValueOfType(key, layout->keys.element_type())) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& storage = Storage(object);
  if (storage.size == 0) {
    return CollectionLookup{};
  }
  const auto table = View(storage, *layout);
  return CollectionLookup{.table = table, .slot = FindSlot(table, *layout, key, HashKey(key))};
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
  return AllocateObject(ObjectKind::Collection, size, [&layout](HeapObject& object) {
    object.SetTypeMetadata(&layout);
    std::construct_at(StorageAfter<CollectionStorage>(object));
  });
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
  if (!CanAllocate(bytes - current.bytes)) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
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
  ReleaseExternalStorage(current.data, current.bytes);
  allocated_bytes_ += bytes;
  committed_bytes_ += bytes;
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
  return InsertCollectionHashed(object, key, HashKey(key), value);
}

std::expected<CollectionInsert, ExecutionStatus> Heap::InsertCollectionHashed(
    HeapObject& object,
    Value key,
    std::size_t hash,
    Value value) noexcept {
  const auto& layout = *object.collection_layout();
  auto& storage = Storage(object);
  Table table;
  Slot slot;
  if (storage.capacity != 0) {
    table = View(storage, layout);
    slot = FindSlot(table, layout, key, hash);
    if (slot.found) {
      if (layout.values) {
        StoreElementStorage(*layout.values, table.values, slot.entry, value);
        if (value.IsReference()) {
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
    table = View(storage, layout);
    slot = FindSlot(table, layout, key, hash);
  }
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
  if (key.IsReference()) {
    WriteBarrier(object, key);
  }
  if (layout.values && value.IsReference()) {
    WriteBarrier(object, value);
  }
  return CollectionInsert::Inserted;
}

std::expected<bool, ExecutionStatus> Heap::HasCollectionKey(const HeapObject& object,
                                                            Value key) noexcept {
  return FindEntry(object, key).transform([](const auto& entry) { return entry.slot.found; });
}

std::expected<Value, ExecutionStatus> Heap::GetCollectionValue(const HeapObject& object,
                                                               Value key) noexcept {
  const auto entry = FindEntry(object, key);
  if (!entry) {
    return std::unexpected(entry.error());
  }
  const auto& layout = *object.collection_layout();
  if (!layout.values) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& slot = entry->slot;
  return slot.found ? LoadElementStorage(*layout.values, entry->table.values, slot.entry)
                    : Value::Null();
}

std::expected<CollectionRemove, ExecutionStatus> Heap::RemoveCollection(HeapObject& object,
                                                                        Value key) noexcept {
  const auto entry = FindEntry(object, key);
  if (!entry) {
    return std::unexpected(entry.error());
  }
  const auto& slot = entry->slot;
  if (!slot.found) {
    return CollectionRemove::Missing;
  }
  auto& storage = Storage(object);
  const auto& table = entry->table;
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
  RootRange root(*this, &rooted, 1, RootOwner::Runtime);
  const auto& right_storage = Storage(right);
  const auto right_table = View(right_storage, layout);
  const auto append = [&](const HeapObject& input) -> std::expected<void, ExecutionStatus> {
    const auto& storage = Storage(input);
    if (storage.size == 0) {
      return {};
    }
    const auto table = View(storage, layout);
    for (auto entry = storage.head; entry != kEmpty; entry = table.next.subspan(entry).front()) {
      const auto key = LoadElementStorage(layout.keys, table.keys, entry);
      const auto hash = table.hashes.subspan(entry).front();
      if (operation != SetOperation::Union) {
        const auto found =
            right_storage.size != 0 && FindSlot(right_table, layout, key, hash).found;
        if (found != (operation == SetOperation::Intersection)) {
          continue;
        }
      }
      const auto inserted = InsertCollectionHashed(*result, key, hash, Value::Null());
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

std::expected<CollectionIterator, ExecutionStatus> Heap::IterateCollection(
    const HeapObject& object) noexcept {
  if (object.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& storage = Storage(object);
  const auto table = View(storage, *object.collection_layout());
  CollectionIterator iterator;
  iterator.object_ = &object;
  iterator.mutation_ = storage.mutation;
  iterator.next_ = storage.head;
  iterator.keys_ = table.keys;
  iterator.values_ = table.values;
  iterator.links_ = table.next;
  return iterator;
}

std::expected<std::optional<CollectionEntry>, ExecutionStatus> CollectionIterator::Next() noexcept {
  if (object_ == nullptr) {
    return std::nullopt;
  }
  if (Storage(*object_).mutation != mutation_) {
    return std::unexpected(ExecutionStatus::ConcurrentModification);
  }
  if (next_ == kEmpty) {
    return std::nullopt;
  }
  const auto& layout = *Heap::CollectionTypeOf(*object_);
  const auto index = next_;
  next_ = links_.subspan(index).front();
  return CollectionEntry{
      .key = LoadElementStorage(layout.keys, keys_, index),
      .value = layout.values ? LoadElementStorage(*layout.values, values_, index) : Value::Null(),
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
  const auto& storage = Storage(object);
  if (storage.size == 0) {
    return;
  }
  const auto table = View(storage, layout);
  const auto mark = [this, &storage, &table](auto elements) {
    for (auto entry = storage.head; entry != kEmpty; entry = table.next.subspan(entry).front()) {
      MarkReference(elements.subspan(entry).front());
    }
  };
  VisitReferenceStorage(layout.keys, table.keys, mark);
  if (layout.values) {
    VisitReferenceStorage(*layout.values, table.values, mark);
  }
}

void Heap::ReleaseCollection(HeapObject& object) noexcept {
  auto& storage = Storage(object);
  ReleaseExternalStorage(storage.data, storage.bytes);
  std::destroy_at(&storage);
}

}  // namespace vela::runtime
