#include "runtime/heap/element_storage.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

#include "base/inline_layout.h"
#include "runtime/heap/field_storage.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {
namespace {

template <class T>
T Load(std::span<const std::byte> storage) noexcept {
  T value;
  std::memcpy(static_cast<void*>(&value), storage.data(), sizeof(T));
  return value;
}

template <class T>
void Store(std::span<std::byte> storage, T value) noexcept {
  std::memcpy(storage.data(), static_cast<const void*>(&value), sizeof(T));
}

}  // namespace

Value LoadInlineSlot(InlineSlotKind kind, std::span<const std::byte> storage) noexcept {
  switch (kind) {
    case InlineSlotKind::Boolean:
      return Value::Boolean(Load<bool>(storage));
    case InlineSlotKind::Integer:
      return Value::Integer(Load<std::int64_t>(storage));
    case InlineSlotKind::Double:
      return Value::Double(Load<double>(storage));
    case InlineSlotKind::Reference: {
      auto* reference = Load<HeapObject*>(storage);
      return reference == nullptr ? Value::Null() : Value::Object(reference);
    }
    case InlineSlotKind::String: {
      auto* reference = Load<StringObject*>(storage);
      return reference == nullptr ? Value::Null() : Value::String(reference);
    }
    case InlineSlotKind::Dynamic:
      return Load<Value>(storage);
    case InlineSlotKind::NullableBoolean:
      return Load<bool>(storage) ? Value::Boolean(Load<bool>(storage.subspan(1))) : Value::Null();
    case InlineSlotKind::NullableInteger:
      return Load<bool>(storage)
                 ? Value::Integer(Load<std::int64_t>(storage.subspan(kInlineWordBytes)))
                 : Value::Null();
    case InlineSlotKind::NullableDouble:
      return Load<bool>(storage) ? Value::Double(Load<double>(storage.subspan(kInlineWordBytes)))
                                 : Value::Null();
  }
  std::unreachable();
}

void StoreInlineSlot(InlineSlotKind kind, std::span<std::byte> storage, Value value) noexcept {
  switch (kind) {
    case InlineSlotKind::Boolean:
      Store(storage, value.AsBoolean());
      break;
    case InlineSlotKind::Integer:
      Store(storage, value.AsInteger());
      break;
    case InlineSlotKind::Double:
      Store(storage, value.AsDouble());
      break;
    case InlineSlotKind::Reference:
      Store(storage, value.IsNull() ? nullptr : value.AsObject());
      break;
    case InlineSlotKind::String:
      Store(storage, value.IsNull() ? nullptr : value.AsString());
      break;
    case InlineSlotKind::Dynamic:
      Store(storage, value);
      break;
    case InlineSlotKind::NullableBoolean:
      Store(storage, !value.IsNull());
      Store(storage.subspan(1), !value.IsNull() && value.AsBoolean());
      break;
    case InlineSlotKind::NullableInteger:
      Store(storage, !value.IsNull());
      Store(storage.subspan(kInlineWordBytes), value.IsNull() ? std::int64_t{} : value.AsInteger());
      break;
    case InlineSlotKind::NullableDouble:
      Store(storage, !value.IsNull());
      Store(storage.subspan(kInlineWordBytes), value.IsNull() ? 0.0 : value.AsDouble());
      break;
  }
}

void StoreInlineElement(const ArrayLayout& layout,
                        std::span<std::byte> storage,
                        std::size_t index,
                        Value value) noexcept {
  const auto& owner = *layout.value_layout;
  const auto& shape = owner.value_layout();
  const auto stride = layout.element_size();
  const auto record = storage.subspan(index * stride, stride);
  if (layout.element.nullable) {
    Store(record, !value.IsNull());
  }
  const auto payload = record.subspan(shape.PayloadOffset(layout.element.nullable));
  if (value.IsNull()) {
    std::memset(payload.data(), 0, payload.size());
    return;
  }
  const auto fields = ObjectFieldStorage(*value.AsObject());
  const auto slots = shape.slots();
  for (std::size_t field = 0; field < slots.size(); ++field) {
    const auto& slot = slots[field];
    StoreInlineSlot(
        slot.kind, payload.subspan(slot.offset),
        FieldStorage::Load(fields, owner.field_count(), static_cast<std::uint32_t>(field)));
  }
}

bool IncludesInlineElement(const ArrayLayout& layout,
                           std::span<const std::byte> storage,
                           std::size_t start,
                           Value value) noexcept {
  const auto& owner = *layout.value_layout;
  const auto& shape = owner.value_layout();
  const auto stride = layout.element_size();
  storage = storage.subspan(start * stride);
  const bool nullable = layout.element.nullable;
  if (value.IsNull()) {
    for (std::size_t offset = 0; offset < storage.size(); offset += stride) {
      if (storage[offset] == std::byte{}) {
        return true;
      }
    }
    return false;
  }
  const auto payload_offset = shape.PayloadOffset(nullable);
  const auto fields = ObjectFieldStorage(*value.AsObject());
  const auto field_count = owner.field_count();
  const auto slots = shape.slots();
  // 第一槽是每个候选的共同筛选条件；固定源值只解码一次，不引入分配或 GC。
  const auto first = slots.empty() ? Value::Null() : FieldStorage::Load(fields, field_count, 0);
  for (std::size_t offset = 0; offset < storage.size(); offset += stride) {
    if (nullable && storage[offset] == std::byte{}) {
      continue;
    }
    const auto payload = storage.subspan(offset + payload_offset);
    if (!slots.empty() &&
        !IsSameValueZero(LoadInlineSlot(slots.front().kind, payload.subspan(slots.front().offset)),
                         first)) {
      continue;
    }
    bool matches = true;
    for (std::size_t field = 1; field < slots.size(); ++field) {
      const auto& slot = slots[field];
      if (!IsSameValueZero(
              LoadInlineSlot(slot.kind, payload.subspan(slot.offset)),
              FieldStorage::Load(fields, field_count, static_cast<std::uint32_t>(field)))) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return true;
    }
  }
  return false;
}

std::size_t ArrayLayout::element_size() const noexcept {
  if (value_layout != nullptr) {
    return value_layout->value_layout().Stride(element.nullable);
  }
  if (record_fields != 0) {
    return static_cast<std::size_t>(record_fields) * FieldStorage::kPayloadBytes;
  }
  if (element.kind == ValueKind::Null && !HasTaggedElements()) {
    return 0;
  }
  return VisitElementType(*this, []<class T> { return sizeof(T); });
}

}  // namespace vela::runtime
