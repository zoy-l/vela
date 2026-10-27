#include "runtime/element_storage.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

std::size_t ArrayLayout::element_size() const noexcept {
  if (element.kind == ValueKind::Null && !HasTaggedElements()) {
    return 0;
  }
  return VisitElementType(*this, []<class T> { return sizeof(T); });
}

Value LoadElementStorage(const ArrayLayout& layout,
                         std::span<const std::byte> storage,
                         std::size_t index) noexcept {
  return VisitElementStorage(layout, storage, [index]<class T>(std::span<const T> elements) {
    const auto value = elements.subspan(index).front();
    if constexpr (std::is_same_v<T, Value>) {
      return value;
    } else if constexpr (std::is_same_v<T, bool>) {
      return Value::Boolean(value);
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
      return Value::Integer(value);
    } else if constexpr (std::is_same_v<T, double>) {
      return Value::Double(value);
    } else if constexpr (std::is_same_v<T, StringObject*>) {
      return value == nullptr ? Value::Null() : Value::String(value);
    } else {
      return value == nullptr ? Value::Null() : Value::Object(value);
    }
  });
}

void StoreElementStorage(const ArrayLayout& layout,
                         std::span<std::byte> storage,
                         std::size_t index,
                         Value value) noexcept {
  VisitElementStorage(layout, storage, [index, value]<class T>(std::span<T> elements) {
    elements.subspan(index).front() = ElementValue<T>(value);
  });
}

}  // namespace vela::runtime
