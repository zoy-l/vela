#include "runtime/heap/heap.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <utility>

#include "base/execution_status.h"
#include "runtime/heap/host_memory.h"
#include "runtime/value/host_object.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

std::expected<HostConstruction, HostObjectError> Heap::PrepareHostConstruction(
    const ClassLayout& layout,
    std::size_t size,
    std::size_t alignment,
    HostObjectStorage::Destructor destroy) noexcept {
  HostMemory extra(*this, 0U);
  if (!extra.IsReserved()) {
    return std::unexpected(HostObjectError::OutOfMemory);
  }
  constexpr auto kPrefix = sizeof(HeapObject) + sizeof(ConstructedHostStorage);
  const auto padding = alignment <= alignof(std::max_align_t)
                           ? (alignment - (kPrefix % alignment)) % alignment
                           : alignment - 1U;
  const auto bytes = kPrefix + padding + size;
  if (bytes > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(HostObjectError::OutOfMemory);
  }
  MaybeCollect(bytes);
  const auto allocation = AllocateRaw(bytes);
  if (allocation.memory == nullptr) {
    return std::unexpected(HostObjectError::OutOfMemory);
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::Host;
  object->layout = &layout;
  object->auxiliary_length = static_cast<std::uint32_t>(size);
  object->allocation_size = allocation.size;
  object->size_class = allocation.size_class;
  auto* storage =
      std::construct_at(StorageAfter<ConstructedHostStorage>(*object), std::move(extra), destroy);
  void* payload = StorageAfter<std::byte>(*storage);
  auto capacity = static_cast<std::size_t>(allocation.size) - kPrefix;
  object->SetNativePayload(std::align(alignment, size, payload, capacity));
  return HostConstruction(*this, *object);
}

std::expected<HeapObject*, HostObjectError> Heap::PublishHostConstruction(
    HostConstruction construction) noexcept {
  if (!construction.IsOwnedBy(*this)) {
    return std::unexpected(HostObjectError::ForeignIsolate);
  }
  auto* object = construction.object_;
  // 构造期间 C++ 可以重入并推进 GC；发布前才准备弱索引，准备后不再触发回收。
  MaybeCollect(0U);
  if (!host_index_.PrepareInsert()) {
    return std::unexpected(HostObjectError::OutOfMemory);
  }
  host_index_.Insert(*object);
  construction.object_ = nullptr;
  Publish(*object,
          {.memory = object, .size = object->allocation_size, .size_class = object->size_class});
  return object;
}

void Heap::DiscardHostConstruction(HeapObject& object) noexcept {
  auto* storage = StorageAfter<ConstructedHostStorage>(object);
  storage->destroy(object.native_payload());
  std::destroy_at(storage);
  const auto size = object.allocation_size;
  const auto class_index = object.size_class;
  allocated_bytes_ -= size;
  std::destroy_at(&object);
  ReleaseBlock({.memory = &object, .size = size, .size_class = class_index});
}

HeapObject* Heap::FindHostObject(const NominalType& type, const void* pointer) noexcept {
  return host_index_.Find(type, pointer);
}

std::expected<HeapObject*, HostObjectError> Heap::WrapHostObject(
    const ClassLayout& layout,
    std::size_t native_size,
    HostObjectStorage storage) noexcept {
  const auto& type = layout.nominal_type();
  auto* pointer = storage.pointer();
  const auto& shared = storage.shared_;
  if (storage.destroy_ == nullptr && shared.use_count() == 0) {
    return std::unexpected(HostObjectError::InvalidSharedOwner);
  }
  if (auto* existing = host_index_.Find(type, pointer)) {
    if (storage.destroy_ != nullptr) {
      // 重复的 unique_ptr 声明了已有对象的第二份所有权；保留原所有者，不执行第二次删除。
      storage.destroy_ = nullptr;
      return std::unexpected(HostObjectError::OwnershipConflict);
    }
    if (existing->auxiliary_length != 0U) {
      return std::unexpected(HostObjectError::OwnershipConflict);
    }
    const auto& previous = *StorageAfter<HostObjectStorage>(*existing);
    const auto& previous_shared = previous.shared_;
    if (previous.destroy_ != nullptr || shared.owner_before(previous_shared) ||
        previous_shared.owner_before(shared)) {
      return std::unexpected(HostObjectError::OwnershipConflict);
    }
    return existing;
  }
  storage.memory_ = HostMemory(*this, native_size);
  if (!storage.memory_.IsReserved()) {
    return std::unexpected(HostObjectError::OutOfMemory);
  }
  constexpr auto kWrapperSize = sizeof(HeapObject) + sizeof(HostObjectStorage);
  // GC 可在清除最后一个旧包装时释放弱索引；准备索引后到发布之间不得再触发 GC。
  MaybeCollect(kWrapperSize);
  if (!host_index_.PrepareInsert()) {
    return std::unexpected(HostObjectError::OutOfMemory);
  }
  const auto allocation = AllocateRaw(kWrapperSize);
  if (allocation.memory == nullptr) {
    host_index_.ReleaseEmpty();
    return std::unexpected(HostObjectError::OutOfMemory);
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::Host;
  object->layout = &layout;
  object->SetNativePayload(pointer);
  std::construct_at(StorageAfter<HostObjectStorage>(*object), std::move(storage));
  host_index_.Insert(*object);
  Publish(*object, allocation);
  return object;
}

void Heap::ReleaseHostObject(HeapObject& object) noexcept {
  host_index_.Erase(object);
  if (object.auxiliary_length != 0U) {
    auto* storage = StorageAfter<ConstructedHostStorage>(object);
    storage->destroy(object.native_payload());
    std::destroy_at(storage);
  } else {
    std::destroy_at(StorageAfter<HostObjectStorage>(object));
  }
}

std::expected<void, ExecutionStatus> Heap::SetHostMemory(HeapObject& object,
                                                         std::size_t bytes) noexcept {
  auto value = Value::Object(&object);
  const RootRange roots(*this, &value, 1);
  if (object.auxiliary_length != 0U) {
    return StorageAfter<ConstructedHostStorage>(object)->memory.Resize(bytes -
                                                                       object.auxiliary_length);
  }
  return StorageAfter<HostObjectStorage>(object)->memory_.Resize(bytes);
}

}  // namespace vela::runtime
