#include "runtime/heap/heap.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace vela::runtime {

HeapObject* Heap::AllocateValueSlots(std::size_t count) noexcept {
  if (count > std::numeric_limits<std::uint32_t>::max() ||
      count > (std::numeric_limits<std::size_t>::max() - sizeof(HeapObject)) / sizeof(Value)) {
    return nullptr;
  }
  const auto bytes = count * sizeof(Value);
  MaybeCollect(sizeof(HeapObject) + bytes);
  auto* slots = static_cast<Value*>(AllocateExternal(bytes));
  if (slots == nullptr) {
    return nullptr;
  }
  std::uninitialized_value_construct_n(slots, count);
  const auto allocation = AllocateRaw(sizeof(HeapObject));
  if (allocation.memory == nullptr) {
    ReleaseExternalStorage(slots, bytes);
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::ValueSlots;
  object->length = static_cast<std::uint32_t>(count);
  object->native_payload = slots;
  Publish(*object, allocation);
  return object;
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
