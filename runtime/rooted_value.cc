#include "runtime/rooted_value.h"

#include <memory>
#include "runtime/isolate.h"
#include "runtime/value.h"

namespace vela::runtime {

RootedValue::RootedValue(Isolate& isolate, Value value)
    : storage_(std::make_shared<Storage>(isolate.heap(), value)) {}

auto RootedValue::value() const noexcept -> Value {
  return storage_->value;
}

}  // namespace vela::runtime
