#include "runtime/heap/heap.h"

#include <cstddef>
#include <expected>
#include <memory>
#include <utility>

#include "base/execution_status.h"
#include "runtime/heap/host_memory.h"
#include "runtime/value/host_object.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

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
  if (storage.memory_.IsReserved() && !storage.memory_.IsOwnedBy(*this)) {
    return std::unexpected(HostObjectError::ForeignIsolate);
  }
  if (storage.destroy_ == nullptr && shared.use_count() == 0) {
    return std::unexpected(HostObjectError::InvalidSharedOwner);
  }
  if (auto* existing = host_index_.Find(type, pointer)) {
    if (storage.destroy_ != nullptr) {
      // 重复的 unique_ptr 声明了已有对象的第二份所有权；保留原所有者，不执行第二次删除。
      storage.destroy_ = nullptr;
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
  if (!storage.memory_.IsReserved()) {
    storage.memory_ = HostMemory(*this, native_size);
    if (!storage.memory_.IsReserved()) {
      return std::unexpected(HostObjectError::OutOfMemory);
    }
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
  object->native_payload = pointer;
  std::construct_at(StorageAfter<HostObjectStorage>(*object), std::move(storage));
  host_index_.Insert(type, pointer, *object);
  Publish(*object, allocation);
  return object;
}

void Heap::ReleaseHostObject(HeapObject& object) noexcept {
  host_index_.Erase(object.layout->nominal_type(), object.native_payload);
  std::destroy_at(StorageAfter<HostObjectStorage>(object));
}

std::expected<void, ExecutionStatus> Heap::SetHostMemory(HeapObject& object,
                                                         std::size_t bytes) noexcept {
  auto value = Value::Object(&object);
  const RootRange roots(*this, &value, 1);
  return StorageAfter<HostObjectStorage>(object)->memory_.Resize(bytes);
}

}  // namespace vela::runtime
