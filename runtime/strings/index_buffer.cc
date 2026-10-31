#include "runtime/strings/index_buffer.h"

#include <cstddef>
#include <limits>
#include <new>

namespace vela::runtime {

std::size_t* IndexBuffer::Allocate(std::size_t count) noexcept {
  if (count > std::numeric_limits<std::size_t>::max() / sizeof(std::size_t)) {
    return nullptr;
  }
  return static_cast<std::size_t*>(::operator new(count * sizeof(std::size_t), std::nothrow));
}

void IndexBuffer::Free(std::size_t* data) noexcept {
  ::operator delete(data);
}

}  // namespace vela::runtime
