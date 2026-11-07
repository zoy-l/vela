#include "runtime/heap/heap.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
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

struct CollectionStorage final {
  std::uint64_t mutation = 0;
  std::byte* data = nullptr;
  std::size_t bytes = 0;
  // 视图借用本缓冲区；分配和扩容同时构造，普通读写不再计算分区或元素宽度。
  Table table;
  std::uint32_t capacity = 0;
  std::uint32_t used = 0;
  std::uint32_t size = 0;
  std::uint32_t head = kEmpty;
  std::uint32_t tail = kEmpty;
  // 只保留命中桶号，不借用查询 key。setter 消费一次并验证当前桶中的真实 key；
  // 删除、clear 和重新插入不会让旧提示更新其他条目，扩容随整个存储一起清空提示。
  std::uint32_t read_bucket = kEmpty;
};

template <typename T>
std::span<T> Section(std::span<std::byte> bytes, std::size_t offset, std::size_t count) noexcept {
  void* data = bytes.subspan(offset, count * sizeof(T)).data();
  return {static_cast<T*>(data), count};
}

Table View(std::span<std::byte> bytes, const BufferLayout& layout, std::size_t capacity) noexcept {
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
      const auto& object = *value.AsObject();
      const auto* layout = Heap::ClassOf(object);
      if (layout != nullptr && layout->IsEnum()) {
        return absl::HashOf(&layout->nominal_type(), EnumOrdinal(object));
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
    const auto entry = table.buckets[bucket];
    if (entry == kEmpty) {
      return {.bucket = deleted == kEmpty ? bucket : deleted, .entry = kEmpty, .found = false};
    }
    if (entry == kDeleted) {
      if (deleted == kEmpty) {
        deleted = bucket;
      }
    } else if (table.hashes[entry] == hash &&
               IsSameKey(LoadElementStorage(layout.keys, table.keys, entry), key)) {
      return {.bucket = bucket, .entry = entry, .found = true};
    }
    bucket = (bucket + 1) & mask;
  }
}

struct CollectionLookup final {
  // 查找本身不分配；仅由本次 Get/Remove 借用，不能跨扩容或 GC 保留。
  const Table* table = nullptr;
  Slot slot;
};

CollectionLookup FindTypedEntry(const HeapObject& object,
                                const CollectionLayout& layout,
                                Value key) noexcept {
  const auto& storage = *StorageAfter<CollectionStorage>(object);
  if (storage.size == 0) {
    return {};
  }
  const auto& table = storage.table;
  return {.table = &table, .slot = FindSlot(table, layout, key, HashKey(key))};
}

std::expected<CollectionLookup, ExecutionStatus> FindEntry(const HeapObject& object,
                                                           Value key) noexcept {
  const auto* layout = Heap::CollectionTypeOf(object);
  if (layout == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (!Heap::IsValueOfType(key, layout->keys.element)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return FindTypedEntry(object, *layout, key);
}

Value ReadMapEntry(const CollectionLayout& layout, const CollectionLookup& entry) noexcept {
  const auto& values = layout.values;
  if (!values) {
    std::unreachable();
  }
  const auto& slot = entry.slot;
  return slot.found ? LoadElementStorage(*values, entry.table->values, slot.entry) : Value::Null();
}

void LinkTail(CollectionStorage& storage, const Table& table, std::uint32_t entry) noexcept {
  table.previous[entry] = storage.tail;
  table.next[entry] = kEmpty;
  if (storage.tail != kEmpty) {
    table.next[storage.tail] = entry;
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
  auto& current = *StorageAfter<CollectionStorage>(object);
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
  const auto buffer = LayoutFor(layout, capacity);
  const auto bytes = buffer.bytes;
  MaybeCollect(bytes);
  auto* memory = static_cast<std::byte*>(AllocateExternal(bytes, current.bytes));
  if (memory == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  CollectionStorage replacement{
      .mutation = current.mutation,
      .data = memory,
      .bytes = bytes,
      .table = View({memory, bytes}, buffer, capacity),
      .capacity = capacity,
  };
  const auto& target = replacement.table;
  std::ranges::fill(target.buckets, kEmpty);
  if (current.size != 0) {
    const auto& source = current.table;
    for (auto entry = current.head; entry != kEmpty; entry = source.next[entry]) {
      const auto next = replacement.used++;
      StoreElementStorage(layout.keys, target.keys, next,
                          LoadElementStorage(layout.keys, source.keys, entry));
      if (layout.values) {
        StoreElementStorage(*layout.values, target.values, next,
                            LoadElementStorage(*layout.values, source.values, entry));
      }
      const auto hash = source.hashes[entry];
      target.hashes[next] = hash;
      const auto mask = static_cast<std::uint32_t>(target.buckets.size() - 1);
      auto bucket = static_cast<std::uint32_t>(hash) & mask;
      while (target.buckets[bucket] != kEmpty) {
        bucket = (bucket + 1) & mask;
      }
      target.buckets[bucket] = next;
      LinkTail(replacement, target, next);
      ++replacement.size;
    }
  }
  ReleaseExternalStorage(current.data, current.bytes);
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
  if (!IsValueOfType(key, layout.keys.element) ||
      (layout.values && !IsValueOfType(value, layout.values->element))) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return InsertCollectionHashed(object, key, HashKey(key), value);
}

std::expected<void, ExecutionStatus> Heap::StoreMapValue(HeapObject& object,
                                                         Value key,
                                                         Value value) noexcept {
  auto& storage = *StorageAfter<CollectionStorage>(object);
  const auto bucket = std::exchange(storage.read_bucket, kEmpty);
  const auto& table = storage.table;
  if (bucket != kEmpty) {
    const auto entry = table.buckets[bucket];
    const auto& layout = *object.collection_layout();
    if (entry < storage.used &&
        IsSameKey(LoadElementStorage(layout.keys, table.keys, entry), key)) {
      StoreCollectionEntry(object, entry, value);
      return {};
    }
  }
  return InsertCollectionHashed(object, key, HashKey(key), value).transform([](auto) {});
}

Value Heap::LoadMapValue(HeapObject& object, Value key) noexcept {
  const auto& layout = *object.collection_layout();
  const auto entry = FindTypedEntry(object, layout, key);
  StorageAfter<CollectionStorage>(object)->read_bucket =
      entry.slot.found ? entry.slot.bucket : kEmpty;
  return ReadMapEntry(layout, entry);
}

Value Heap::ResolveMapValue(HeapObject& object, Value key, MapLookup& lookup) noexcept {
  const auto& layout = *object.collection_layout();
  const auto& storage = *StorageAfter<CollectionStorage>(object);
  const auto hash = HashKey(key);
  const auto slot = storage.capacity == 0 ? Slot{} : FindSlot(storage.table, layout, key, hash);
  const auto& values = *layout.values;
  auto* integer =
      slot.found && !values.HasTaggedElements() && values.element.kind == ValueKind::Integer
          ? &ElementSlots<std::int64_t>(storage.table.values)[slot.entry]
          : nullptr;
  lookup = {.hash = hash, .bucket = slot.bucket, .entry = slot.entry, .integer_value = integer};
  return ReadMapEntry(layout, {.table = &storage.table, .slot = slot});
}

std::expected<void, ExecutionStatus> Heap::StoreMapValue(HeapObject& object,
                                                         Value key,
                                                         Value value,
                                                         MapLookup lookup) noexcept {
  return InsertCollectionLocated(object, key, value, lookup).transform([](auto) {});
}

void Heap::StoreCollectionEntry(HeapObject& object, std::uint32_t entry, Value value) noexcept {
  const auto& layout = *object.collection_layout();
  const auto& values = layout.values;
  if (!values) {
    std::unreachable();
  }
  StoreElementStorage(*values, StorageAfter<CollectionStorage>(object)->table.values, entry, value);
  if (value.IsReference()) {
    WriteBarrier(object, value);
  }
}

std::expected<CollectionInsert, ExecutionStatus> Heap::InsertCollectionHashed(
    HeapObject& object,
    Value key,
    std::size_t hash,
    Value value) noexcept {
  const auto& layout = *object.collection_layout();
  auto& storage = *StorageAfter<CollectionStorage>(object);
  const auto& table = storage.table;
  Slot slot;
  if (storage.capacity != 0) {
    slot = FindSlot(table, layout, key, hash);
  }
  return InsertCollectionLocated(
      object, key, value,
      {.hash = hash, .bucket = slot.bucket, .entry = slot.entry, .integer_value = nullptr});
}

std::expected<CollectionInsert, ExecutionStatus> Heap::InsertCollectionLocated(
    HeapObject& object,
    Value key,
    Value value,
    MapLookup lookup) noexcept {
  const auto& layout = *object.collection_layout();
  auto& storage = *StorageAfter<CollectionStorage>(object);
  const auto& table = storage.table;
  const auto hash = lookup.hash;
  Slot slot{.bucket = lookup.bucket, .entry = lookup.entry, .found = lookup.entry != kEmpty};
  if (slot.found) {
    if (layout.values) {
      StoreCollectionEntry(object, slot.entry, value);
    }
    return CollectionInsert::Updated;
  }
  if (storage.used == storage.capacity) {
    const auto grown = GrowCollection(object);
    if (!grown) {
      return std::unexpected(grown.error());
    }
    slot = FindSlot(table, layout, key, hash);
  }
  const auto entry = storage.used++;
  StoreElementStorage(layout.keys, table.keys, entry, key);
  if (layout.values) {
    StoreElementStorage(*layout.values, table.values, entry, value);
  }
  table.hashes[entry] = hash;
  table.buckets[slot.bucket] = entry;
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
  return FindCollectionValue(object, key).transform([](const auto& value) {
    return value.value_or(Value::Null());
  });
}

std::expected<std::optional<Value>, ExecutionStatus> Heap::FindCollectionValue(
    const HeapObject& object,
    Value key) noexcept {
  const auto entry = FindEntry(object, key);
  if (!entry) {
    return std::unexpected(entry.error());
  }
  const auto& layout = *object.collection_layout();
  if (!layout.values) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (!entry->slot.found) {
    return std::nullopt;
  }
  return ReadMapEntry(layout, *entry);
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
  auto& storage = *StorageAfter<CollectionStorage>(object);
  const auto& table = *entry->table;
  const auto previous = table.previous[slot.entry];
  const auto next = table.next[slot.entry];
  if (previous == kEmpty) {
    storage.head = next;
  } else {
    table.next[previous] = next;
  }
  if (next == kEmpty) {
    storage.tail = previous;
  } else {
    table.previous[next] = previous;
  }
  table.buckets[slot.bucket] = kDeleted;
  --storage.size;
  ++storage.mutation;
  return CollectionRemove::Removed;
}

std::expected<void, ExecutionStatus> Heap::ClearCollection(HeapObject& object) noexcept {
  if (object.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& storage = *StorageAfter<CollectionStorage>(object);
  if (storage.size != 0) {
    ++storage.mutation;
  }
  if (storage.capacity != 0) {
    std::ranges::fill(storage.table.buckets, kEmpty);
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
  RootRange root(*this, &rooted, 1, std::nullopt);
  const auto& right_storage = *StorageAfter<CollectionStorage>(right);
  const auto& right_table = right_storage.table;
  const auto append = [&](const HeapObject& input) -> std::expected<void, ExecutionStatus> {
    const auto& storage = *StorageAfter<CollectionStorage>(input);
    if (storage.size == 0) {
      return {};
    }
    const auto& table = storage.table;
    for (auto entry = storage.head; entry != kEmpty; entry = table.next[entry]) {
      const auto key = LoadElementStorage(layout.keys, table.keys, entry);
      const auto hash = table.hashes[entry];
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
  return StorageAfter<CollectionStorage>(object)->size;
}

std::expected<CollectionIterator, ExecutionStatus> Heap::IterateCollection(
    const HeapObject& object) noexcept {
  if (object.kind != ObjectKind::Collection) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& storage = *StorageAfter<CollectionStorage>(object);
  const auto& table = storage.table;
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
  if (StorageAfter<CollectionStorage>(*object_)->mutation != mutation_) {
    return std::unexpected(ExecutionStatus::ConcurrentModification);
  }
  if (next_ == kEmpty) {
    return std::nullopt;
  }
  const auto& layout = *Heap::CollectionTypeOf(*object_);
  const auto index = next_;
  next_ = links_[index];
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
  const auto& storage = *StorageAfter<CollectionStorage>(object);
  const auto length = ArrayLength(array);
  if (!length || *length != storage.size) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto* element_layout = layout.element_layout(projection);
  if (element_layout == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (storage.size == 0) {
    return {};
  }
  const auto& table = storage.table;
  const auto elements = projection == CollectionProjection::Keys ? table.keys : table.values;
  std::uint32_t index = 0;
  for (auto entry = storage.head; entry != kEmpty; entry = table.next[entry]) {
    const auto value = LoadElementStorage(*element_layout, elements, entry);
    if (!SetElement(array, index++, value)) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  return {};
}

void Heap::MarkCollection(HeapObject& object) noexcept {
  const auto& layout = *object.collection_layout();
  const auto& storage = *StorageAfter<CollectionStorage>(object);
  if (storage.size == 0) {
    return;
  }
  const auto& table = storage.table;
  const auto mark = [this, &storage, &table](auto elements) {
    for (auto entry = storage.head; entry != kEmpty; entry = table.next[entry]) {
      MarkReference(elements[entry]);
    }
  };
  VisitReferenceStorage(layout.keys, table.keys, mark);
  if (layout.values) {
    VisitReferenceStorage(*layout.values, table.values, mark);
  }
}

void Heap::ReleaseCollection(HeapObject& object) noexcept {
  auto& storage = *StorageAfter<CollectionStorage>(object);
  ReleaseExternalStorage(storage.data, storage.bytes);
  std::destroy_at(&storage);
}

}  // namespace vela::runtime
