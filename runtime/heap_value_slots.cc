#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/value.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>

namespace vela::runtime {

HeapObject* Heap::AllocateValueSlots(std::size_t count) noexcept {
  if (count > std::numeric_limits<std::uint32_t>::max() ||
      count > (std::numeric_limits<std::size_t>::max() - sizeof(HeapObject)) / sizeof(Value)) {
    return nullptr;
  }
  const auto bytes = count * sizeof(Value);
  MaybeCollect(sizeof(HeapObject) + bytes);
  auto* slots = static_cast<Value*>(::operator new(count * sizeof(Value), std::nothrow));
  if (slots == nullptr) {
    return nullptr;
  }
  std::uninitialized_value_construct_n(slots, count);
  const auto allocation = AllocateRaw(sizeof(HeapObject));
  if (allocation.memory == nullptr) {
    std::destroy_n(slots, count);
    ::operator delete(slots);
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::ValueSlots;
  object->length = static_cast<std::uint32_t>(count);
  object->native_payload = slots;
  allocated_bytes_ += bytes;
  committed_bytes_ += bytes;
  Publish(*object, allocation);
  return object;
}

std::expected<void, ExecutionStatus> Heap::ExtendValueSlots(HeapObject& object,
                                                            std::size_t count) noexcept {
  assert(object.kind == ObjectKind::ValueSlots && count >= object.length);
  if (count == object.length) {
    return {};
  }
  if (count > std::numeric_limits<std::uint32_t>::max() ||
      count > std::numeric_limits<std::size_t>::max() / sizeof(Value)) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto extra = (count - object.length) * sizeof(Value);
  MaybeCollect(extra);
  auto* slots = static_cast<Value*>(::operator new(count * sizeof(Value), std::nothrow));
  if (slots == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  std::uninitialized_value_construct_n(slots, count);
  std::ranges::copy(ValueSlots(object), slots);
  std::destroy_n(static_cast<Value*>(object.native_payload), object.length);
  ::operator delete(object.native_payload);
  object.native_payload = slots;
  object.length = static_cast<std::uint32_t>(count);
  allocated_bytes_ += extra;
  committed_bytes_ += extra;
  return {};
}

std::span<Value> Heap::ValueSlots(HeapObject& object) noexcept {
  assert(object.kind == ObjectKind::ValueSlots);
  return {static_cast<Value*>(object.native_payload), object.length};
}

void Heap::SetValueSlot(HeapObject& object, std::size_t index, Value value) noexcept {
  ValueSlots(object).subspan(index).front() = value;
  WriteBarrier(object, value);
}

}  // namespace vela::runtime
