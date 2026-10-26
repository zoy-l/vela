#include "runtime/heap.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

struct ByteView final {
  HeapObject* owner;
  std::uint32_t offset;
};

ByteView& ViewStorage(HeapObject& object) {
  void* storage = std::span(&object, 1).last(0).data();
  return *static_cast<ByteView*>(storage);
}

}  // namespace

bool Heap::IsByteArray(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::ByteArray;
}

bool Heap::IsByteData(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::ByteData;
}

bool Heap::IsIntegerArray(const HeapObject& object) noexcept {
  if (object.kind != ObjectKind::Array) {
    return false;
  }
  const auto& layout = *object.array_layout();
  return layout.element.kind == ValueKind::Integer && !layout.element.nullable;
}

std::span<std::uint8_t> Heap::ByteArrayBytes(HeapObject& object) noexcept {
  assert(IsByteArray(object));
  void* storage = std::span(&object, 1).last(0).data();
  return {static_cast<std::uint8_t*>(storage), object.length};
}

std::span<std::uint8_t> Heap::ByteDataBytes(HeapObject& object) noexcept {
  assert(IsByteData(object));
  const auto& view = ViewStorage(object);
  return ByteArrayBytes(*view.owner).subspan(view.offset, object.length);
}

HeapObject* Heap::AllocateByteArray(std::size_t length) noexcept {
  constexpr auto kMaximumLength = std::numeric_limits<std::uint32_t>::max() - sizeof(HeapObject);
  if (length > kMaximumLength) {
    return nullptr;
  }
  const auto size = sizeof(HeapObject) + length;
  MaybeCollect(size);
  const auto allocation = AllocateRaw(size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::ByteArray;
  object->length = static_cast<std::uint32_t>(length);
  std::ranges::fill(ByteArrayBytes(*object), 0);
  Publish(*object, allocation.size);
  return object;
}

HeapObject* Heap::AllocateByteData(HeapObject& owner, ByteViewRange range) noexcept {
  const auto input = Value::Object(&owner);
  RootRange root(*this, &input, 1);
  const auto size = sizeof(HeapObject) + sizeof(ByteView);
  MaybeCollect(size);
  const auto allocation = AllocateRaw(size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::ByteData;
  object->length = range.length;
  std::construct_at(&ViewStorage(*object), &owner, range.offset);
  // 所有引用在 Publish 前初始化，沿用新对象发布时的 GC 屏障。
  Publish(*object, allocation.size);
  return object;
}

void Heap::MarkByteData(HeapObject& object) noexcept {
  MarkObject(*ViewStorage(object).owner);
}

}  // namespace vela::runtime
