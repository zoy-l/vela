#include "runtime/heap/heap.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <vector>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "runtime/heap/field_storage.h"
#include "runtime/heap/instance_storage.h"
#include "runtime/heap/intrusive_link_internal.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

std::size_t InstanceFieldBytes(std::uint32_t count) noexcept {
  return sizeof(InstanceStorage) + FieldStorage::Size(count) + (count * sizeof(FieldState));
}

}  // namespace

std::optional<std::uint32_t> ClassLayout::FieldIndex(DeclarationId field) const noexcept {
  const auto found = field_indices_.find(field);
  return found == field_indices_.end() ? std::nullopt : std::optional(found->second);
}

std::span<std::byte> ObjectFieldStorage(HeapObject& object) noexcept {
  if (object.layout->storage() == ClassStorage::Reloadable) {
    return static_cast<InstanceStorage*>(object.native_payload)->fields();
  }
  return {StorageAfter<std::byte>(object), FieldStorage::Size(object.auxiliary_length)};
}

std::span<const std::byte> ObjectFieldStorage(const HeapObject& object) noexcept {
  if (object.layout->storage() == ClassStorage::Reloadable) {
    return static_cast<const InstanceStorage*>(object.native_payload)->fields();
  }
  return {StorageAfter<std::byte>(object), FieldStorage::Size(object.auxiliary_length)};
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
  storage->unready = count;
  const auto fields = storage->fields();
  static_assert(static_cast<std::uint8_t>(ValueKind::Null) == 0);
  std::uninitialized_fill_n(fields.data(), fields.size(), std::byte{0});
  std::uninitialized_fill_n(storage->states().data(), count, FieldState::Uninitialized);
  return storage;
}

void Heap::ReleaseInstanceStorage(InstanceStorage& storage) noexcept {
  const auto count = storage.count;
  instance_field_bytes_ -= InstanceFieldBytes(count);
  std::destroy_n(storage.states().data(), count);
  std::destroy_at(&storage);
  FreeNative(&storage);
}

void Heap::RegisterInstance(HeapObject& object, InstanceStorage& storage) noexcept {
  auto& first = object.layout->nominal_->instances_;
  if (first == nullptr) {
    ++instance_type_count_;
  }
  storage.object = &object;
  internal::PrependIntrusive(storage, first, &InstanceStorage::next,
                             &InstanceStorage::previous_link);
}

void Heap::UnregisterInstance(HeapObject& object, InstanceStorage& storage) noexcept {
  auto& first = object.layout->nominal_->instances_;
  internal::UnlinkIntrusive(storage, &InstanceStorage::next, &InstanceStorage::previous_link);
  if (first == nullptr) {
    --instance_type_count_;
  }
}

void Heap::ReleaseInstance(HeapObject& object) noexcept {
  if (object.layout->storage() == ClassStorage::Inline) {
    return;
  }
  auto& storage = *static_cast<InstanceStorage*>(object.native_payload);
  UnregisterInstance(object, storage);
  ReleaseInstanceStorage(storage);
}

void Heap::AppendInstances(const NominalType& type, std::vector<Value>& instances) {
  for (auto* storage = type.instances_; storage != nullptr; storage = storage->next) {
    instances.push_back(Value::Object(storage->object));
  }
}

InstanceStorage& Heap::InstanceFields(HeapObject& object) noexcept {
  return *static_cast<InstanceStorage*>(object.native_payload);
}

std::optional<Value> Heap::GetField(const HeapObject& object, std::uint32_t index) noexcept {
  const auto* layout = object.layout;
  if (layout == nullptr || index >= layout->field_count()) {
    return std::nullopt;
  }
  return LoadField(object, index);
}

Value Heap::LoadField(const HeapObject& object, std::uint32_t index) noexcept {
  return FieldStorage::Load(ObjectFieldStorage(object), object.layout->field_count(), index);
}

std::expected<void, ExecutionStatus> Heap::SetField(HeapObject& object,
                                                    std::uint32_t index,
                                                    Value value) noexcept {
  const auto* layout = object.layout;
  if (layout == nullptr || index >= layout->field_count()) {
    return std::unexpected(ExecutionStatus::InvalidField);
  }
  FieldStorage::Store(ObjectFieldStorage(object), layout->field_count(), index, value);
  if (layout->storage() == ClassStorage::Reloadable) {
    InstanceFields(object).SetFieldState(index, FieldState::Ready);
  }
  if (value.IsReference()) {
    WriteBarrier(object, value);
  }
  return {};
}

InstanceMigration::InstanceMigration(Heap& heap,
                                     HeapObject& object,
                                     const ClassLayout& layout) noexcept
    : heap_(&heap),
      object_(Value::Object(&object)),
      layout_(&layout),
      object_root_(heap, &object_, 1, std::nullopt),
      field_roots_(std::nullopt) {}

InstanceMigration::~InstanceMigration() {
  if (storage_ != nullptr) {
    field_roots_.Detach();
    heap_->ReleaseInstanceStorage(*storage_);
  }
}

std::expected<std::unique_ptr<InstanceMigration>, ExecutionStatus> Heap::PrepareInstanceMigration(
    HeapObject& object,
    const ClassLayout& layout) noexcept {
  std::unique_ptr<InstanceMigration> migration(new (std::nothrow)
                                                   InstanceMigration(*this, object, layout));
  if (migration == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  auto* storage = AllocateInstanceStorage(layout.field_count());
  if (storage == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  migration->storage_ = storage;
  auto& previous = InstanceFields(object);
  const auto old_fields = previous.fields();
  const auto old_states = previous.states();
  const auto fields = storage->fields();
  const auto declarations = layout.field_declarations();
  const auto old_declarations = object.layout->field_declarations();
  for (std::uint32_t index = 0; index < previous.count; ++index) {
    const auto field = declarations.subspan(index).front();
    if (field != DeclarationId{} && field == old_declarations.subspan(index).front()) {
      FieldStorage::Store(fields, storage->count, index,
                          FieldStorage::Load(old_fields, previous.count, index));
      storage->SetFieldState(index, old_states.subspan(index).front());
    }
  }
  migration->field_roots_.Attach(*this, *storage);
  return migration;
}

void InstanceMigration::Commit() noexcept {
  auto& object = *object_.AsObject();
  auto& previous = Heap::InstanceFields(object);
  auto& storage = *storage_;
  heap_->UnregisterInstance(object, previous);
  object.native_payload = &storage;
  object.layout = layout_;
  heap_->RegisterInstance(object, storage);
  const auto fields = storage.fields();
  for (std::uint32_t index = 0; index < storage.count; ++index) {
    heap_->WriteBarrier(object, FieldStorage::Load(fields, storage.count, index));
  }
  heap_->ReleaseInstanceStorage(previous);
  field_roots_.Detach();
  storage_ = nullptr;
}

}  // namespace vela::runtime
