#include "runtime/value/host_object.h"

#include <memory>
#include <utility>

#include "runtime/heap/heap.h"
#include "runtime/heap/host_memory.h"
#include "runtime/value/object.h"

namespace vela::runtime {

HostConstruction::HostConstruction(Heap& heap, HeapObject& object) noexcept
    : heap_(&heap), object_(&object) {}

HostConstruction::~HostConstruction() {
  if (object_ != nullptr) {
    heap_->DiscardHostConstruction(*object_);
  }
}

HostConstruction::HostConstruction(HostConstruction&& other) noexcept
    : heap_(other.heap_), object_(std::exchange(other.object_, nullptr)) {}

void* HostConstruction::pointer() const noexcept {
  return HostObjectPointer(*object_);
}

HostObjectStorage::HostObjectStorage(void* pointer, Destructor destroy) noexcept
    : pointer_(pointer), destroy_(destroy) {}

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
