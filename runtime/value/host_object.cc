#include "runtime/value/host_object.h"

#include <memory>
#include <utility>

#include "runtime/heap/host_memory.h"

namespace vela::runtime {

HostObjectStorage::HostObjectStorage(void* pointer, Destructor destroy, HostMemory memory) noexcept
    : memory_(std::move(memory)), pointer_(pointer), destroy_(destroy) {}

HostObjectStorage::HostObjectStorage(std::shared_ptr<void> shared) noexcept
    : pointer_(shared.get()), shared_(std::move(shared)) {}

HostObjectStorage::~HostObjectStorage() {
  if (destroy_ != nullptr) {
    destroy_(pointer_);
  }
}

HostObjectStorage::HostObjectStorage(HostObjectStorage&& other) noexcept
    : memory_(std::move(other.memory_)),
      pointer_(std::exchange(other.pointer_, nullptr)),
      destroy_(std::exchange(other.destroy_, nullptr)),
      shared_(std::move(other.shared_)) {}

}  // namespace vela::runtime
