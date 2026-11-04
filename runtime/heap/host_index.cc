#include "runtime/heap/host_index.h"

#include <cassert>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <span>

#include <absl/hash/hash.h>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"

namespace vela::runtime {
namespace {

constexpr std::size_t kInitialCapacity = 16;
constexpr std::size_t kLoadFactorDivisor = 2;

std::size_t Bucket(const NominalType* type, const void* pointer, std::size_t capacity) {
  return absl::HashOf(type, pointer) & (capacity - 1);
}

}  // namespace

HostIndex::~HostIndex() {
  heap_->FreeNative(entries_);
}

HostIndex::Entry* HostIndex::FindEntry(const NominalType& type, const void* pointer) noexcept {
  if (capacity_ == 0) {
    return nullptr;
  }
  const auto entries = std::span(entries_, capacity_);
  auto index = Bucket(&type, pointer, capacity_);
  while (true) {
    auto& entry = entries[index];
    if (entry.state == State::Empty) {
      return nullptr;
    }
    if (entry.state == State::Live && entry.type == &type && entry.pointer == pointer) {
      return &entry;
    }
    index = (index + 1) & (capacity_ - 1);
  }
}

HeapObject* HostIndex::Find(const NominalType& type, const void* pointer) noexcept {
  const auto* entry = FindEntry(type, pointer);
  return entry != nullptr ? entry->object : nullptr;
}

HostIndex::State HostIndex::InsertEntry(Entry* entries,
                                        std::size_t capacity,
                                        Entry value) noexcept {
  const auto slots = std::span(entries, capacity);
  auto index = Bucket(value.type, value.pointer, capacity);
  while (slots[index].state == State::Live) {
    index = (index + 1) & (capacity - 1);
  }
  const auto previous = slots[index].state;
  slots[index] = value;
  return previous;
}

std::expected<void, ExecutionStatus> HostIndex::PrepareInsert() noexcept {
  if (capacity_ != 0 && size_ + removed_ + 1 <= capacity_ / kLoadFactorDivisor) {
    return {};
  }
  auto capacity = capacity_ == 0 ? kInitialCapacity : capacity_;
  if (size_ + 1 > capacity / kLoadFactorDivisor) {
    if (capacity > std::numeric_limits<std::size_t>::max() / kLoadFactorDivisor) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    capacity *= kLoadFactorDivisor;
  }
  if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(Entry)) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  auto* prepared = static_cast<Entry*>(heap_->AllocateNative(capacity * sizeof(Entry)));
  if (prepared == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  std::uninitialized_value_construct_n(prepared, capacity);
  // AllocateNative 可能已经回收并移除了旧条目；只迁移分配返回后仍存活的记录。
  for (const auto& entry : std::span(entries_, capacity_)) {
    if (entry.state == State::Live) {
      InsertEntry(prepared, capacity, entry);
    }
  }
  heap_->FreeNative(entries_);
  entries_ = prepared;
  capacity_ = capacity;
  removed_ = 0;
  return {};
}

void HostIndex::Insert(const NominalType& type, const void* pointer, HeapObject& object) noexcept {
  const auto previous =
      InsertEntry(entries_, capacity_,
                  {.type = &type, .pointer = pointer, .object = &object, .state = State::Live});
  if (previous == State::Removed) {
    --removed_;
  }
  ++size_;
}

void HostIndex::Erase(const NominalType& type, const void* pointer) noexcept {
  auto* entry = FindEntry(type, pointer);
  assert(entry != nullptr);
  entry->state = State::Removed;
  entry->type = nullptr;
  entry->pointer = nullptr;
  entry->object = nullptr;
  --size_;
  ++removed_;
  ReleaseEmpty();
}

void HostIndex::ReleaseEmpty() noexcept {
  if (size_ == 0) {
    heap_->FreeNative(entries_);
    entries_ = nullptr;
    capacity_ = 0;
    removed_ = 0;
  }
}

}  // namespace vela::runtime
