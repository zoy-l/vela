#include "runtime/heap.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
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
  return *StorageAfter<ByteView>(object);
}

}  // namespace

bool Heap::IsByteArray(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::ByteArray;
}

bool Heap::IsByteData(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::ByteData;
}

std::span<std::uint8_t> Heap::ByteArrayBytes(HeapObject& object) noexcept {
  assert(IsByteArray(object));
  return {StorageAfter<std::uint8_t>(object), object.length};
}

std::span<std::uint8_t> Heap::ByteDataBytes(HeapObject& object) noexcept {
  assert(IsByteData(object));
  const auto& view = ViewStorage(object);
  return ByteArrayBytes(*view.owner).subspan(view.offset, object.length);
}

HeapObject* Heap::AllocateByteArray(std::size_t length) noexcept {
  if (length > kMaximumByteArrayLength) {
    return nullptr;
  }
  const auto size = sizeof(HeapObject) + length;
  return AllocateObject(ObjectKind::ByteArray, size, [length](HeapObject& object) {
    object.length = static_cast<std::uint32_t>(length);
    std::ranges::fill(ByteArrayBytes(object), 0);
  });
}

HeapObject* Heap::AllocateByteData(HeapObject& owner, ByteViewRange range) noexcept {
  const auto input = Value::Object(&owner);
  RootRange root(*this, &input, 1);
  const auto size = sizeof(HeapObject) + sizeof(ByteView);
  return AllocateObject(ObjectKind::ByteData, size, [&owner, range](HeapObject& object) {
    object.length = range.length;
    std::construct_at(StorageAfter<ByteView>(object), &owner, range.offset);
  });
}

void Heap::MarkByteData(HeapObject& object) noexcept {
  MarkObject(*ViewStorage(object).owner);
}

}  // namespace vela::runtime
