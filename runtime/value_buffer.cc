#include "runtime/value_buffer.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <new>

#include "runtime/value.h"

namespace vela::runtime {

ValueBuffer::ValueBuffer(std::size_t count) noexcept : count_(count) {
  if (count == 0 || count > std::numeric_limits<std::size_t>::max() / sizeof(Value)) {
    return;
  }
  data_ = static_cast<Value*>(::operator new(count * sizeof(Value), std::nothrow));
  if (data_ != nullptr) {
    std::uninitialized_value_construct_n(data_, count);
  }
}

ValueBuffer::~ValueBuffer() {
  if (data_ != nullptr) {
    std::destroy_n(data_, count_);
    ::operator delete(data_);
  }
}

}  // namespace vela::runtime
