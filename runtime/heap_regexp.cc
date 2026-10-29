#include "runtime/heap.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <new>

#include <pcre2.h>

#include "runtime/object.h"
#include "runtime/regexp_storage.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

struct alignas(std::max_align_t) NativeHeader final {
  std::size_t size;
};

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
  return StorageAfter<std::byte>(*header);
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
  return *StorageAfter<RegExpStorage>(object);
}

RegExpMatchStorage& Heap::RegExpMatchOf(HeapObject& object) noexcept {
  assert(IsRegExpMatch(object));
  return *StorageAfter<RegExpMatchStorage>(object);
}

HeapObject* Heap::AllocateRegExp(const RegExpStorage& storage) noexcept {
  const auto source = Value::String(storage.source);
  RootRange root(*this, &source, 1, RootOwner::Runtime);
  const auto size = sizeof(HeapObject) + sizeof(RegExpStorage);
  return AllocateObject(ObjectKind::RegExp, size, [&storage](HeapObject& object) {
    std::construct_at(StorageAfter<RegExpStorage>(object), storage);
  });
}

HeapObject* Heap::AllocateRegExpMatch(const RegExpMatchStorage& storage) noexcept {
  constexpr auto kHeader = sizeof(HeapObject) + sizeof(RegExpMatchStorage);
  if (storage.capture_count >
      (std::numeric_limits<std::uint32_t>::max() - kHeader) / sizeof(RegExpCapture)) {
    return nullptr;
  }
  const auto size = kHeader + (storage.capture_count * sizeof(RegExpCapture));
  return AllocateObject(ObjectKind::RegExpMatch, size, [&storage](HeapObject& object) {
    auto* match = std::construct_at(StorageAfter<RegExpMatchStorage>(object), storage);
    std::ranges::uninitialized_default_construct(match->captures());
  });
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
