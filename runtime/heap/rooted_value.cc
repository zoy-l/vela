#include "runtime/heap/rooted_value.h"

#include <memory>
#include "runtime/isolate/isolate.h"
#include "runtime/value/value.h"

namespace vela::runtime {

RootedValue::RootedValue(Isolate& isolate, Value value)
    : storage_(std::make_shared<Storage>(isolate.heap(), value)) {}

Value RootedValue::value() const noexcept {
  return storage_->value;
}

}  // namespace vela::runtime
