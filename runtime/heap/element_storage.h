#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

template <class Visitor>
decltype(auto) VisitElementType(const ArrayLayout& layout, const Visitor& visitor) {
  if (layout.HasTaggedElements()) {
    return visitor.template operator()<Value>();
  }
  switch (layout.element.kind) {
    case ValueKind::Boolean:
      return visitor.template operator()<bool>();
    case ValueKind::Integer:
      return visitor.template operator()<std::int64_t>();
    case ValueKind::Double:
      return visitor.template operator()<double>();
    case ValueKind::String:
      return visitor.template operator()<StringObject*>();
    case ValueKind::Object:
      return visitor.template operator()<HeapObject*>();
    case ValueKind::Null:
      std::unreachable();
  }
  std::unreachable();
}

template <class T, class Byte>
[[nodiscard]] auto ElementSlots(std::span<Byte> storage) noexcept {
  using Element = std::conditional_t<std::is_const_v<Byte>, const T, T>;
  using Bytes = std::conditional_t<std::is_const_v<Byte>, const void, void>;
  Bytes* bytes = storage.data();
  return std::span(static_cast<Element*>(bytes), storage.size() / sizeof(T));
}

template <class Byte, class Visitor>
decltype(auto) VisitElementStorage(const ArrayLayout& layout,
                                   std::span<Byte> storage,
                                   const Visitor& visitor) {
  return VisitElementType(layout, [&storage, &visitor]<class T> -> decltype(auto) {
    return visitor(ElementSlots<T>(storage));
  });
}

// 选择表示只发生一次；visitor 接收 tagged Value、对象指针或字符串指针的连续视图。
template <class Byte, class Visitor>
void VisitReferenceStorage(const ArrayLayout& layout,
                           std::span<Byte> storage,
                           const Visitor& visitor) {
  if (!layout.HasReferences()) {
    return;
  }
  VisitElementStorage(layout, storage, [&visitor]<class T>(std::span<T> elements) {
    using Element = std::remove_const_t<T>;
    if constexpr (std::is_same_v<Element, Value> || std::is_pointer_v<Element>) {
      visitor(elements);
    }
  });
}

template <class T>
[[nodiscard]] T ElementValue(Value value) noexcept {
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

// 调用者保证存储对齐、索引有效及写入值满足布局类型；这里仅负责连续存储的表示。
[[nodiscard]] Value LoadElementStorage(const ArrayLayout& layout,
                                       std::span<const std::byte> storage,
                                       std::size_t index) noexcept;
void StoreElementStorage(const ArrayLayout& layout,
                         std::span<std::byte> storage,
                         std::size_t index,
                         Value value) noexcept;

}  // namespace vela::runtime
