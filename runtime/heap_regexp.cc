#include "runtime/heap.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <span>

#include <pcre2.h>

#include "runtime/object.h"
#include "runtime/regexp_storage.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

struct alignas(std::max_align_t) NativeHeader final {
  std::size_t size;
};

template <typename T>
T& Storage(HeapObject& object) noexcept {
  void* storage = std::span(&object, 1).last(0).data();
  return *static_cast<T*>(storage);
}

}  // namespace

void* Heap::AllocateNative(std::size_t size) noexcept {
  if (size > std::numeric_limits<std::size_t>::max() - sizeof(NativeHeader)) {
    return nullptr;
  }
  const auto total = size + sizeof(NativeHeader);
  MaybeCollect(total);
  auto* header = static_cast<NativeHeader*>(::operator new(total, std::nothrow));
  if (header == nullptr) {
    return nullptr;
  }
  std::construct_at(header, total);
  allocated_bytes_ += total;
  committed_bytes_ += total;
  return std::span(header, 1).last(0).data();
}

void Heap::FreeNative(void* pointer) noexcept {
  if (pointer == nullptr) {
    return;
  }
  auto* header = std::prev(static_cast<NativeHeader*>(pointer));
  allocated_bytes_ -= header->size;
  committed_bytes_ -= header->size;
  std::destroy_at(header);
  ::operator delete(header);
}

bool Heap::IsRegExp(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::RegExp;
}

bool Heap::IsRegExpMatch(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::RegExpMatch;
}

RegExpStorage& Heap::RegExpOf(HeapObject& object) noexcept {
  assert(IsRegExp(object));
  return Storage<RegExpStorage>(object);
}

RegExpMatchStorage& Heap::RegExpMatchOf(HeapObject& object) noexcept {
  assert(IsRegExpMatch(object));
  return Storage<RegExpMatchStorage>(object);
}

HeapObject* Heap::AllocateRegExp(const RegExpStorage& storage) noexcept {
  const auto source = Value::String(storage.source);
  RootRange root(*this, &source, 1);
  const auto size = sizeof(HeapObject) + sizeof(RegExpStorage);
  MaybeCollect(size);
  const auto allocation = AllocateRaw(size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::RegExp;
  std::construct_at(&Storage<RegExpStorage>(*object), storage);
  Publish(*object, allocation.size);
  return object;
}

HeapObject* Heap::AllocateRegExpMatch(const RegExpMatchStorage& storage,
                                      std::span<const RegExpCapture> captures) noexcept {
  constexpr auto kHeader = sizeof(HeapObject) + sizeof(RegExpMatchStorage);
  if (captures.size() >
      (std::numeric_limits<std::uint32_t>::max() - kHeader) / sizeof(RegExpCapture)) {
    return nullptr;
  }
  const auto size = kHeader + captures.size_bytes();
  MaybeCollect(size);
  const auto allocation = AllocateRaw(size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::RegExpMatch;
  auto* match = std::construct_at(&Storage<RegExpMatchStorage>(*object), storage);
  std::ranges::uninitialized_copy(captures, match->captures());
  Publish(*object, allocation.size);
  return object;
}

void Heap::MarkRegExp(HeapObject& object) noexcept {
  if (IsRegExp(object)) {
    MarkValue(Value::String(RegExpOf(object).source));
    return;
  }
  const auto& match = RegExpMatchOf(object);
  MarkObject(*match.pattern);
  MarkValue(Value::String(match.input));
}

void Heap::ReleaseRegExp(HeapObject& object) noexcept {
  pcre2_code_free(RegExpOf(object).code);
}

}  // namespace vela::runtime
