#include "runtime/heap.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <type_traits>
#include <utility>

#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

std::span<std::byte> Storage(HeapObject& array, const ArrayLayout& layout, std::size_t length) {
  void* bytes = std::span(&array, 1).last(0).data();
  return {static_cast<std::byte*>(bytes), layout.element_size() * length};
}

template <class T>
std::span<T> Elements(std::span<std::byte> storage) {
  void* bytes = storage.data();
  return {static_cast<T*>(bytes), storage.size() / sizeof(T)};
}

template <class Visitor>
void VisitStorage(const ArrayLayout& layout, std::span<std::byte> storage, const Visitor& visitor) {
  if (layout.HasTaggedElements()) {
    visitor(Elements<Value>(storage));
    return;
  }
  switch (layout.element_kind) {
    case ValueKind::Boolean:
      visitor(Elements<bool>(storage));
      return;
    case ValueKind::Integer:
      visitor(Elements<std::int64_t>(storage));
      return;
    case ValueKind::Double:
      visitor(Elements<double>(storage));
      return;
    case ValueKind::String:
      visitor(Elements<StringObject*>(storage));
      return;
    case ValueKind::Object:
      visitor(Elements<HeapObject*>(storage));
      return;
    case ValueKind::Null:
      std::unreachable();
  }
  std::unreachable();
}

template <class T>
T ElementValue(Value value) {
  if constexpr (std::is_same_v<T, Value>) {
    return value;
  } else if constexpr (std::is_same_v<T, bool>) {
    return value.AsBoolean();
  } else if constexpr (std::is_same_v<T, std::int64_t>) {
    return value.AsInteger();
  } else if constexpr (std::is_same_v<T, double>) {
    return value.AsDouble();
  } else if constexpr (std::is_same_v<T, StringObject*>) {
    return value.IsNull() ? nullptr : value.AsString();
  } else {
    return value.IsNull() ? nullptr : value.AsObject();
  }
}

}  // namespace

std::expected<HeapObject*, ArrayAccessError> Heap::SliceArray(HeapObject& array,
                                                              std::uint32_t start,
                                                              std::uint32_t end) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  if (start > end || end > array.length) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  const auto source = Value::Object(&array);
  RootRange roots(*this, &source, 1);
  const auto& layout = *array.array_layout();
  auto* copy = AllocateArray(layout, end - start);
  if (copy == nullptr) {
    return std::unexpected(ArrayAccessError::OutOfMemory);
  }
  const auto count = static_cast<std::size_t>(end - start);
  if (count != 0) {
    const auto input = Storage(array, layout, array.length)
                           .subspan(start * layout.element_size(), count * layout.element_size());
    const auto output = Storage(*copy, layout, count);
    std::memcpy(output.data(), input.data(), input.size());
    if (phase_ != GcPhase::Idle) {
      ProcessGrayObject(*copy);
    }
  }
  return copy;
}

std::expected<void, ArrayAccessError> Heap::FillArray(HeapObject& array, Value value) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  const auto& layout = *array.array_layout();
  if (!IsValueOfType(value, layout.element_type())) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  const auto storage = Storage(array, layout, array.length);
  VisitStorage(layout, storage, [value]<class T>(std::span<T> elements) -> void {
    std::ranges::fill(elements, ElementValue<T>(value));
  });
  WriteBarrier(array, value);
  return {};
}

std::expected<void, ArrayAccessError> Heap::ReverseArray(HeapObject& array) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  const auto& layout = *array.array_layout();
  const auto storage = Storage(array, layout, array.length);
  VisitStorage(layout, storage, [](auto elements) -> void { std::ranges::reverse(elements); });
  return {};
}

}  // namespace vela::runtime
