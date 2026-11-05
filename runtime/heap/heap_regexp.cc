#include <optional>
#include "runtime/heap/heap.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>

#include <pcre2.h>

#include "runtime/regexp/regexp_storage.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
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
  RootRange root(*this, &source, 1, std::nullopt);
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
