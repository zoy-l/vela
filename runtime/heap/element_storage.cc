#include "runtime/heap/element_storage.h"

#include <cstddef>

#include "runtime/heap/field_storage.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

std::size_t ArrayLayout::element_size() const noexcept {
  if (record_fields != 0) {
    return static_cast<std::size_t>(record_fields) * FieldStorage::kPayloadBytes;
  }
  if (element.kind == ValueKind::Null && !HasTaggedElements()) {
    return 0;
  }
  return VisitElementType(*this, []<class T> { return sizeof(T); });
}

}  // namespace vela::runtime
