#include "runtime/heap/heap.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <vector>

#include "base/declaration.h"
#include "runtime/heap/instance_storage.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

std::size_t InstanceFieldBytes(std::uint32_t count) noexcept {
  return sizeof(InstanceStorage) + (count * (sizeof(Value) + sizeof(FieldState)));
}

}  // namespace

std::optional<std::uint32_t> ClassLayout::FieldIndex(DeclarationId field) const noexcept {
  const auto found = field_indices_.find(field);
  return found == field_indices_.end() ? std::nullopt : std::optional(found->second);
}

Value* ObjectFields(HeapObject* object) noexcept {
  if (object->layout->storage() == ClassStorage::Reloadable) {
    return static_cast<InstanceStorage*>(object->native_payload)->values().data();
  }
  return StorageAfter<Value>(*object);
}

const Value* ObjectFields(const HeapObject* object) noexcept {
  if (object->layout->storage() == ClassStorage::Reloadable) {
    return static_cast<const InstanceStorage*>(object->native_payload)->values().data();
  }
  return StorageAfter<Value>(*object);
}

InstanceStorage* Heap::AllocateInstanceStorage(std::uint32_t count) noexcept {
  static_assert(sizeof(std::size_t) > sizeof(count));
  const auto bytes = InstanceFieldBytes(count);
  auto* memory = AllocateNative(bytes);
  if (memory == nullptr) {
    return nullptr;
  }
  instance_field_bytes_ += bytes;
  auto* storage = std::construct_at(static_cast<InstanceStorage*>(memory));
  storage->count = count;
  std::uninitialized_value_construct_n(storage->values().data(), count);
  std::uninitialized_fill_n(storage->states().data(), count, FieldState::Uninitialized);
  return storage;
}

void Heap::ReleaseInstanceStorage(InstanceStorage& storage) noexcept {
  const auto count = storage.count;
  instance_field_bytes_ -= InstanceFieldBytes(count);
  std::destroy_n(storage.values().data(), count);
  std::destroy_n(storage.states().data(), count);
  std::destroy_at(&storage);
  FreeNative(&storage);
}

void Heap::RegisterInstance(HeapObject& object, InstanceStorage& storage) {
  auto& first = instances_.try_emplace(&object.layout->nominal_type(), nullptr).first->second;
  storage.object = &object;
  storage.next = first;
  if (first != nullptr) {
    first->previous = &storage;
  }
  first = &storage;
}

void Heap::ReleaseInstance(HeapObject& object) noexcept {
  if (object.layout->storage() == ClassStorage::Inline) {
    std::destroy_n(ObjectFields(&object), object.layout->field_count());
    return;
  }
  auto& storage = *static_cast<InstanceStorage*>(object.native_payload);
  if (storage.previous == nullptr) {
    const auto found = instances_.find(&object.layout->nominal_type());
    if (storage.next == nullptr) {
      instances_.erase(found);
    } else {
      found->second = storage.next;
    }
  } else {
    storage.previous->next = storage.next;
  }
  if (storage.next != nullptr) {
    storage.next->previous = storage.previous;
  }
  ReleaseInstanceStorage(storage);
}

void Heap::AppendInstances(const NominalType& type, std::vector<Value>& instances) const {
  const auto found = instances_.find(&type);
  if (found == instances_.end()) {
    return;
  }
  for (auto* storage = found->second; storage != nullptr; storage = storage->next) {
    instances.push_back(Value::Object(storage->object));
  }
}

InstanceStorage& Heap::InstanceFields(HeapObject& object) noexcept {
  return *static_cast<InstanceStorage*>(object.native_payload);
}

bool Heap::SetField(HeapObject& object, std::uint32_t index, Value value) noexcept {
  auto* field = GetField(object, index);
  if (field == nullptr) {
    return false;
  }
  *field = value;
  if (object.layout->storage() == ClassStorage::Reloadable) {
    InstanceFields(object).states().subspan(index).front() = FieldState::Ready;
  }
  if (value.IsReference()) {
    WriteBarrier(object, value);
  }
  return true;
}

InstanceMigration::InstanceMigration(Heap& heap,
                                     HeapObject& object,
                                     const ClassLayout& layout) noexcept
    : heap_(&heap),
      object_(Value::Object(&object)),
      layout_(&layout),
      object_root_(heap, &object_, 1, RootOwner::Runtime),
      field_roots_(RootOwner::Runtime) {}

InstanceMigration::~InstanceMigration() {
  if (storage_ != nullptr) {
    field_roots_.Detach();
    heap_->ReleaseInstanceStorage(*storage_);
  }
}

std::unique_ptr<InstanceMigration> Heap::PrepareInstanceMigration(
    HeapObject& object,
    const ClassLayout& layout) noexcept {
  std::unique_ptr<InstanceMigration> migration(new (std::nothrow)
                                                   InstanceMigration(*this, object, layout));
  if (migration == nullptr) {
    return nullptr;
  }
  auto* storage = AllocateInstanceStorage(layout.field_count());
  if (storage == nullptr) {
    return nullptr;
  }
  migration->storage_ = storage;
  auto& previous = InstanceFields(object);
  const auto old_fields = previous.values();
  const auto old_states = previous.states();
  const auto fields = storage->values();
  const auto states = storage->states();
  const auto declarations = layout.field_declarations();
  const auto old_declarations = object.layout->field_declarations();
  for (std::size_t index = 0; index < old_fields.size(); ++index) {
    const auto field = declarations.subspan(index).front();
    if (field != DeclarationId{} && field == old_declarations.subspan(index).front()) {
      fields.subspan(index).front() = old_fields.subspan(index).front();
      states.subspan(index).front() = old_states.subspan(index).front();
    }
  }
  migration->field_roots_.Attach(*this, fields.data(), fields.size());
  return migration;
}

void InstanceMigration::Commit() noexcept {
  auto& object = *object_.AsObject();
  auto& previous = Heap::InstanceFields(object);
  auto& storage = *storage_;
  storage.object = &object;
  storage.previous = previous.previous;
  storage.next = previous.next;
  if (storage.previous == nullptr) {
    heap_->instances_.find(&object.layout->nominal_type())->second = &storage;
  } else {
    storage.previous->next = &storage;
  }
  if (storage.next != nullptr) {
    storage.next->previous = &storage;
  }
  object.native_payload = &storage;
  object.layout = layout_;
  for (const auto value : storage.values()) {
    heap_->WriteBarrier(object, value);
  }
  heap_->ReleaseInstanceStorage(previous);
  field_roots_.Detach();
  storage_ = nullptr;
}

}  // namespace vela::runtime
