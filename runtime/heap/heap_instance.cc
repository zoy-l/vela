#include "base/inline_layout.h"
#include "runtime/heap/heap.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>
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

void ClassLayout::BindValueFields(std::vector<ValueField> fields) {
  value_fields_ = std::move(fields);
}

std::span<std::byte> ObjectFieldStorage(HeapObject& object) noexcept {
  if (object.layout->storage() == ClassStorage::Reloadable) {
    return static_cast<InstanceStorage*>(object.native_payload())->fields();
  }
  const auto header = std::as_writable_bytes(std::span(&object, 1));
  return {header.subspan(object.inline_field_offset).data(),
          FieldStorage::Size(object.auxiliary_length)};
}

std::span<const std::byte> ObjectFieldStorage(const HeapObject& object) noexcept {
  if (object.layout->storage() == ClassStorage::Reloadable) {
    return static_cast<const InstanceStorage*>(object.native_payload())->fields();
  }
  const auto header = std::as_bytes(std::span(&object, 1));
  return {header.subspan(object.inline_field_offset).data(),
          FieldStorage::Size(object.auxiliary_length)};
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

void Heap::ReleaseReloadableInstance(HeapObject& object) noexcept {
  auto& storage = *static_cast<InstanceStorage*>(object.native_payload());
  UnregisterInstance(object, storage);
  ReleaseInstanceStorage(storage);
}

void Heap::AppendInstances(const NominalType& type, std::vector<Value>& instances) {
  for (auto* storage = type.instances_; storage != nullptr; storage = storage->next) {
    instances.push_back(Value::Object(storage->object));
  }
}

InstanceStorage& Heap::InstanceFields(HeapObject& object) noexcept {
  return *static_cast<InstanceStorage*>(object.native_payload());
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

std::expected<Value, ExecutionStatus> Heap::ReadValueField(HeapObject& object,
                                                           std::uint32_t index) noexcept {
  if (object.layout->value_fields().empty()) {
    return LoadField(object, index);
  }
  const auto& field = object.layout->value_fields()[index];
  auto offset = field.offset;
  if (field.layout == nullptr) {
    return LoadField(object, offset);
  }
  if (field.nullable && !LoadField(object, offset++).AsBoolean()) {
    return Value::Null();
  }
  const auto source = Value::Object(&object);
  const RootRange root(*this, &source, 1, std::nullopt);
  auto* copy = AllocateInstance(*field.layout);
  if (copy == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto count = field.layout->field_count();
  const auto storage = ObjectFieldStorage(*copy);
  for (std::uint32_t slot = 0; slot < count; ++slot) {
    FieldStorage::Store(storage, count, slot, LoadField(object, offset + slot));
  }
  return Value::Object(copy);
}

void Heap::WriteValueField(HeapObject& object, std::uint32_t index, Value value) noexcept {
  if (object.layout->value_fields().empty()) {
    std::ignore = SetField(object, index, value);
    return;
  }
  const auto& field = object.layout->value_fields()[index];
  auto offset = field.offset;
  if (field.layout == nullptr) {
    std::ignore = SetField(object, offset, value);
    return;
  }
  if (field.nullable) {
    std::ignore = SetField(object, offset++, Value::Boolean(!value.IsNull()));
  }
  const auto slots = field.layout->value_layout().slots();
  if (!object.layout->IsStruct() && slots.empty() && !field.nullable) {
    std::ignore = SetField(object, offset, Value::Boolean(true));
  }
  for (std::uint32_t slot = 0; slot < slots.size(); ++slot) {
    auto leaf = Value::Null();
    if (!value.IsNull()) {
      leaf = LoadField(*value.AsObject(), slot);
    } else {
      // 缺失的嵌套值与 native 聚合的零载荷保持同一规范表示。
      switch (slots[slot].kind) {
        case InlineSlotKind::Boolean:
          leaf = Value::Boolean(false);
          break;
        case InlineSlotKind::Integer:
          leaf = Value::Integer(0);
          break;
        case InlineSlotKind::Double:
          leaf = Value::Double(0.0);
          break;
        default:
          break;
      }
    }
    std::ignore = SetField(object, offset + slot, leaf);
  }
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
  const auto shared_count = std::min(previous.count, storage->count);
  for (std::uint32_t index = 0; index < shared_count; ++index) {
    const auto field = declarations[index];
    if (field != DeclarationId{} && field == old_declarations[index]) {
      FieldStorage::Store(fields, storage->count, index,
                          FieldStorage::Load(old_fields, previous.count, index));
      storage->SetFieldState(index, old_states[index]);
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
  object.SetNativePayload(&storage);
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
