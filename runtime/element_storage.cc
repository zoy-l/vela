#include "runtime/element_storage.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

template <typename T>
std::span<T> Slots(std::span<std::byte> bytes) noexcept {
  void* storage = bytes.data();
  return {static_cast<T*>(storage), bytes.size() / sizeof(T)};
}

template <typename T>
std::span<const T> Slots(std::span<const std::byte> bytes) noexcept {
  const void* storage = bytes.data();
  return {static_cast<const T*>(storage), bytes.size() / sizeof(T)};
}

}  // namespace

Value LoadElementStorage(const ArrayLayout& layout,
                         std::span<const std::byte> storage,
                         std::size_t index) noexcept {
  if (layout.HasTaggedElements()) {
    return Slots<Value>(storage).subspan(index).front();
  }
  switch (layout.element.kind) {
    case ValueKind::Boolean:
      return Value::Boolean(Slots<bool>(storage).subspan(index).front());
    case ValueKind::Integer:
      return Value::Integer(Slots<std::int64_t>(storage).subspan(index).front());
    case ValueKind::Double:
      return Value::Double(Slots<double>(storage).subspan(index).front());
    case ValueKind::String: {
      auto* value = Slots<StringObject*>(storage).subspan(index).front();
      return value == nullptr ? Value::Null() : Value::String(value);
    }
    case ValueKind::Object: {
      auto* value = Slots<HeapObject*>(storage).subspan(index).front();
      return value == nullptr ? Value::Null() : Value::Object(value);
    }
    case ValueKind::Null:
      std::unreachable();
  }
  std::unreachable();
}

void StoreElementStorage(const ArrayLayout& layout,
                         std::span<std::byte> storage,
                         std::size_t index,
                         Value value) noexcept {
  if (layout.HasTaggedElements()) {
    Slots<Value>(storage).subspan(index).front() = value;
    return;
  }
  switch (layout.element.kind) {
    case ValueKind::Boolean:
      Slots<bool>(storage).subspan(index).front() = value.AsBoolean();
      return;
    case ValueKind::Integer:
      Slots<std::int64_t>(storage).subspan(index).front() = value.AsInteger();
      return;
    case ValueKind::Double:
      Slots<double>(storage).subspan(index).front() = value.AsDouble();
      return;
    case ValueKind::String:
      Slots<StringObject*>(storage).subspan(index).front() =
          value.IsNull() ? nullptr : value.AsString();
      return;
    case ValueKind::Object:
      Slots<HeapObject*>(storage).subspan(index).front() =
          value.IsNull() ? nullptr : value.AsObject();
      return;
    case ValueKind::Null:
      std::unreachable();
  }
  std::unreachable();
}

}  // namespace vela::runtime
