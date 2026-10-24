#include "runtime/heap.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

constexpr auto AlignUp(std::size_t value, std::size_t alignment) noexcept -> std::size_t {
  return (value + alignment - 1) & ~(alignment - 1);
}

template <typename T>
auto ArrayElements(HeapObject& object, std::size_t length) noexcept -> std::span<T> {
  void* storage = std::span(&object, 1).last(0).data();
  return {static_cast<T*>(storage), length};
}

template <typename T>
auto ArrayElements(const HeapObject& object, std::size_t length) noexcept -> std::span<const T> {
  const void* storage = std::span(&object, 1).last(0).data();
  return {static_cast<const T*>(storage), length};
}

void InitializeArray(HeapObject& object, ValueKind kind, std::size_t length) noexcept {
  switch (kind) {
    case ValueKind::Boolean:
      std::uninitialized_value_construct_n(ArrayElements<bool>(object, length).data(), length);
      break;
    case ValueKind::Integer:
      std::uninitialized_value_construct_n(ArrayElements<std::int64_t>(object, length).data(),
                                           length);
      break;
    case ValueKind::Double:
      std::uninitialized_value_construct_n(ArrayElements<double>(object, length).data(), length);
      break;
    case ValueKind::String:
      std::uninitialized_value_construct_n(ArrayElements<StringObject*>(object, length).data(),
                                           length);
      break;
    case ValueKind::Object:
      std::uninitialized_value_construct_n(ArrayElements<HeapObject*>(object, length).data(),
                                           length);
      break;
    case ValueKind::Null:
      break;
  }
}

}  // namespace

auto ObjectFields(HeapObject* object) noexcept -> Value* {
  void* storage = std::span(object, 1).last(0).data();
  return static_cast<Value*>(storage);
}

auto ObjectFields(const HeapObject* object) noexcept -> const Value* {
  const void* storage = std::span(object, 1).last(0).data();
  return static_cast<const Value*>(storage);
}

ClassLayout::ClassLayout(std::uint32_t field_count, std::vector<std::uint32_t> reference_fields)
    : field_count_(field_count), reference_fields_(std::move(reference_fields)) {}

auto ArrayLayout::element_size() const noexcept -> std::size_t {
  switch (element_kind) {
    case ValueKind::Boolean:
      return sizeof(bool);
    case ValueKind::Integer:
      return sizeof(std::int64_t);
    case ValueKind::Double:
      return sizeof(double);
    case ValueKind::Object:
      return sizeof(HeapObject*);
    case ValueKind::String:
      return sizeof(StringObject*);
    case ValueKind::Null:
      return 0;
  }
  return 0;
}

RootRange::RootRange(Heap& heap, Value* values, std::size_t count) noexcept {
  Attach(heap, values, count);
}

RootRange::~RootRange() {
  Detach();
}

void RootRange::Attach(Heap& heap, Value* values, std::size_t count) noexcept {
  Detach();
  heap_ = &heap;
  values_ = values;
  count_ = count;
  heap_->RegisterRoot(*this);
}

void RootRange::Detach() noexcept {
  if (heap_ == nullptr) {
    return;
  }
  vela::runtime::Heap::UnregisterRoot(*this);
  heap_ = nullptr;
  values_ = nullptr;
  count_ = 0;
  next_ = nullptr;
  previous_link_ = nullptr;
}

Heap::Heap(std::size_t gc_threshold) noexcept
    : gc_threshold_(std::max<std::size_t>(gc_threshold, 1)),
      minimum_gc_threshold_(std::max<std::size_t>(gc_threshold, 1)) {}

Heap::~Heap() {
  DestroyAll();
}

auto Heap::AllocateInstance(ClassLayout& layout, std::span<const Value> initial_fields) noexcept
    -> HeapObject* {
  if (initial_fields.size() > layout.field_count()) {
    return nullptr;
  }

  const std::size_t requested_size =
      AlignUp(sizeof(HeapObject) + (sizeof(Value) * layout.field_count()), alignof(Value));
  if (requested_size > std::numeric_limits<std::uint32_t>::max()) {
    return nullptr;
  }
  MaybeCollect(requested_size);

  HeapObject* object = AllocateRaw(requested_size);
  if (object == nullptr) {
    return nullptr;
  }

  object->kind = ObjectKind::Instance;
  object->layout = &layout;
  object->gray_next = nullptr;

  Value* fields = ObjectFields(object);
  std::uninitialized_fill_n(fields, layout.field_count(), Value::Null());
  std::ranges::copy(initial_fields, fields);

  if (phase_ != GcPhase::Idle) {
    object->mark_bit = current_mark_bit_;
    if (phase_ == GcPhase::Mark) {
      object->color = 0;
      object->gray_next = gray_;
      gray_ = object;
    } else {
      object->color = 1;
      for (std::uint32_t index : layout.reference_fields()) {
        MarkValue(std::span(fields, layout.field_count()).subspan(index).front());
      }
      DrainGray();
    }
  }

  objects_ = object;
  ++object_count_;
  return object;
}

auto Heap::AllocateArray(const ArrayLayout& layout, std::size_t length) noexcept -> HeapObject* {
  const std::size_t element_size = layout.element_size();
  if (element_size == 0 ||
      length > (std::numeric_limits<std::uint32_t>::max() - sizeof(HeapObject)) / element_size) {
    return nullptr;
  }
  const std::size_t requested_size = sizeof(HeapObject) + (length * element_size);
  MaybeCollect(requested_size);
  HeapObject* object = AllocateRaw(requested_size);
  if (object == nullptr) {
    return nullptr;
  }
  object->array_layout = &layout;
  object->array_length = static_cast<std::uint32_t>(length);
  // payload 与对象头单次分配；按原生元素类型建立对象生命周期。
  InitializeArray(*object, layout.element_kind, length);
  object->kind = ObjectKind::Array;
  object->layout = nullptr;
  object->gray_next = nullptr;
  if (phase_ != GcPhase::Idle) {
    object->mark_bit = current_mark_bit_;
    if (phase_ == GcPhase::Mark) {
      object->color = 0;
      object->gray_next = gray_;
      gray_ = object;
    } else {
      object->color = 1;
    }
  }
  objects_ = object;
  ++object_count_;
  return object;
}

auto Heap::GetField(HeapObject& object, std::uint32_t index) noexcept -> Value* {
  if (object.layout == nullptr || index >= object.layout->field_count()) {
    return nullptr;
  }
  return std::span(ObjectFields(&object), object.layout->field_count()).subspan(index).data();
}

auto Heap::GetField(const HeapObject& object, std::uint32_t index) noexcept -> const Value* {
  if (object.layout == nullptr || index >= object.layout->field_count()) {
    return nullptr;
  }
  return std::span(ObjectFields(&object), object.layout->field_count()).subspan(index).data();
}

auto Heap::SetField(HeapObject& object, std::uint32_t index, Value value) noexcept -> bool {
  Value* field = GetField(object, index);
  if (field == nullptr) {
    return false;
  }
  *field = value;
  WriteBarrier(object, value);
  return true;
}

auto Heap::GetElement(const HeapObject& object, std::uint32_t index) noexcept
    -> std::expected<Value, ArrayAccessError> {
  if (object.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  const auto length = object.array_length;
  if (index >= length) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  switch (object.array_layout->element_kind) {
    case ValueKind::Boolean:
      return Value::Boolean(ArrayElements<bool>(object, length).subspan(index).front());
    case ValueKind::Integer:
      return Value::Integer(ArrayElements<std::int64_t>(object, length).subspan(index).front());
    case ValueKind::Double:
      return Value::Double(ArrayElements<double>(object, length).subspan(index).front());
    case ValueKind::String: {
      auto* value = ArrayElements<StringObject*>(object, length).subspan(index).front();
      return value == nullptr ? Value::Null() : Value::String(value);
    }
    case ValueKind::Object: {
      auto* value = ArrayElements<HeapObject*>(object, length).subspan(index).front();
      return value == nullptr ? Value::Null() : Value::Object(value);
    }
    case ValueKind::Null:
      return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  return std::unexpected(ArrayAccessError::ElementTypeMismatch);
}

auto Heap::SetElement(HeapObject& object, std::uint32_t index, Value value) noexcept
    -> std::expected<void, ArrayAccessError> {
  if (object.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  const auto length = object.array_length;
  if (index >= length) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  const auto& layout = *object.array_layout;
  if (value.kind() != layout.element_kind) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  switch (layout.element_kind) {
    case ValueKind::Boolean:
      ArrayElements<bool>(object, length).subspan(index).front() = value.AsBoolean();
      break;
    case ValueKind::Integer:
      ArrayElements<std::int64_t>(object, length).subspan(index).front() = value.AsInteger();
      break;
    case ValueKind::Double:
      ArrayElements<double>(object, length).subspan(index).front() = value.AsDouble();
      break;
    case ValueKind::String:
      ArrayElements<StringObject*>(object, length).subspan(index).front() = value.AsString();
      break;
    case ValueKind::Object: {
      auto* child = value.AsObject();
      if (child == nullptr ||
          (layout.element_class != nullptr &&
           (child->kind != ObjectKind::Instance || child->layout != layout.element_class)) ||
          (layout.element_array != nullptr &&
           (child->kind != ObjectKind::Array || child->array_layout != layout.element_array))) {
        return std::unexpected(ArrayAccessError::ElementTypeMismatch);
      }
      ArrayElements<HeapObject*>(object, length).subspan(index).front() = child;
      WriteBarrier(object, value);
      break;
    }
    case ValueKind::Null:
      return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  return {};
}

void Heap::StartCollection() noexcept {
  if (phase_ != GcPhase::Idle) {
    return;
  }

  current_mark_bit_ ^= 1;
  gray_ = nullptr;
  sweep_link_ = nullptr;
  phase_ = GcPhase::Mark;
  MarkRoots();
}

void Heap::Step(std::size_t work_budget) noexcept {
  if (work_budget == 0) {
    return;
  }

  while (work_budget != 0 && phase_ != GcPhase::Idle) {
    if (phase_ == GcPhase::Mark) {
      if (gray_ != nullptr) {
        HeapObject* object = gray_;
        gray_ = object->gray_next;
        object->gray_next = nullptr;
        ProcessGrayObject(*object);
        --work_budget;
        continue;
      }
      phase_ = GcPhase::Sweep;
      sweep_link_ = &objects_;
      continue;
    }

    HeapObject* object = *sweep_link_;
    if (object == nullptr) {
      FinishSweep();
      continue;
    }

    if (object->mark_bit != current_mark_bit_) {
      *sweep_link_ = object->next;
      ReleaseRaw(*object);
      --object_count_;
    } else {
      sweep_link_ = &object->next;
    }
    --work_budget;
  }
}

void Heap::FinishCollection() noexcept {
  if (phase_ == GcPhase::Idle) {
    return;
  }
  while (phase_ != GcPhase::Idle) {
    Step(std::numeric_limits<std::size_t>::max());
  }
}

void Heap::Collect() noexcept {
  if (phase_ == GcPhase::Idle) {
    StartCollection();
  }
  FinishCollection();
}

void Heap::SetGcThreshold(std::size_t threshold) noexcept {
  minimum_gc_threshold_ = std::max<std::size_t>(threshold, 1);
  gc_threshold_ = minimum_gc_threshold_;
}

void Heap::RegisterRoot(RootRange& root) noexcept {
  root.previous_link_ = &roots_;
  root.next_ = roots_;
  if (root.next_ != nullptr) {
    root.next_->previous_link_ = &root.next_;
  }
  roots_ = &root;
  if (phase_ != GcPhase::Idle) {
    for (const Value value : std::span(root.values_, root.count_)) {
      MarkValue(value);
    }
    if (phase_ == GcPhase::Sweep) {
      DrainGray();
    }
  }
}

void Heap::UnregisterRoot(RootRange& root) noexcept {
  if (root.previous_link_ == nullptr) {
    return;
  }
  *root.previous_link_ = root.next_;
  if (root.next_ != nullptr) {
    root.next_->previous_link_ = root.previous_link_;
  }
  root.next_ = nullptr;
  root.previous_link_ = nullptr;
}

void Heap::MarkRoots() noexcept {
  for (RootRange* root = roots_; root != nullptr; root = root->next_) {
    for (const Value value : std::span(root->values_, root->count_)) {
      MarkValue(value);
    }
  }
}

void Heap::MarkValue(Value value) noexcept {
  if (value.IsObject() && value.AsObject() != nullptr) {
    MarkObject(*value.AsObject());
  }
}

void Heap::MarkObject(HeapObject& object) noexcept {
  if (object.mark_bit == current_mark_bit_) {
    return;
  }
  object.mark_bit = current_mark_bit_;
  object.color = 0;
  object.gray_next = gray_;
  gray_ = &object;
}

void Heap::ProcessGrayObject(HeapObject& object) noexcept {
  object.color = 1;
  if (object.kind == ObjectKind::Array) {
    if (object.array_layout->element_kind == ValueKind::Object) {
      for (HeapObject* child : ArrayElements<HeapObject*>(object, object.array_length)) {
        if (child != nullptr) {
          MarkObject(*child);
        }
      }
    }
    return;
  }
  if (object.layout == nullptr) {
    return;
  }
  const Value* fields = ObjectFields(&object);
  for (std::uint32_t index : object.layout->reference_fields()) {
    MarkValue(std::span(fields, object.layout->field_count()).subspan(index).front());
  }
}

void Heap::DrainGray() noexcept {
  while (gray_ != nullptr) {
    HeapObject* object = gray_;
    gray_ = object->gray_next;
    object->gray_next = nullptr;
    ProcessGrayObject(*object);
  }
}

void Heap::WriteBarrier(HeapObject& source, Value value) noexcept {
  if (phase_ != GcPhase::Idle && source.mark_bit == current_mark_bit_) {
    MarkValue(value);
    if (phase_ == GcPhase::Sweep) {
      DrainGray();
    }
  }
}

auto Heap::AllocateRaw(std::size_t requested_size) noexcept -> HeapObject* {
  const std::size_t block_size = SelectBlockSize(requested_size);
  const std::size_t class_index = SizeClassIndex(block_size);
  void* memory = nullptr;

  if (class_index < free_blocks_.size() && free_blocks_.at(class_index) != nullptr) {
    HeapObject* block = free_blocks_.at(class_index);
    free_blocks_.at(class_index) = block->next;
    memory = block;
    std::destroy_at(block);
  } else {
    memory = ::operator new(block_size, std::nothrow);
    if (memory == nullptr) {
      return nullptr;
    }
    committed_bytes_ += block_size;
  }

  auto* object = static_cast<HeapObject*>(memory);
  std::construct_at(object);
  object->mark_bit = current_mark_bit_;
  object->allocation_size = static_cast<std::uint32_t>(block_size);
  allocated_bytes_ += block_size;
  object->next = objects_;
  return object;
}

void Heap::ReleaseRaw(HeapObject& object) noexcept {
  const std::size_t block_size = object.allocation_size;
  allocated_bytes_ -= block_size;

  if (object.kind == ObjectKind::Instance && object.layout != nullptr) {
    std::destroy_n(ObjectFields(&object), object.layout->field_count());
  }
  const std::size_t class_index = SizeClassIndex(block_size);
  if (class_index < free_blocks_.size()) {
    object.next = free_blocks_.at(class_index);
    free_blocks_.at(class_index) = &object;
    return;
  }

  committed_bytes_ -= block_size;
  std::destroy_at(&object);
  ::operator delete(&object);
}

auto Heap::SelectBlockSize(std::size_t requested_size) noexcept -> std::size_t {
  for (std::size_t block_size : kSizeClasses) {
    if (requested_size <= block_size) {
      return block_size;
    }
  }
  return requested_size;
}

auto Heap::SizeClassIndex(std::size_t block_size) noexcept -> std::size_t {
  for (std::size_t index = 0; index < kSizeClasses.size(); ++index) {
    if (kSizeClasses.at(index) == block_size) {
      return index;
    }
  }
  return kSizeClasses.size();
}

auto Heap::ShouldCollect(std::size_t next_size) const noexcept -> bool {
  return next_size > gc_threshold_ || allocated_bytes_ > gc_threshold_ - next_size;
}

void Heap::MaybeCollect(std::size_t next_size) noexcept {
  if (phase_ == GcPhase::Idle && ShouldCollect(next_size)) {
    StartCollection();
  }
  if (phase_ != GcPhase::Idle) {
    Step(kAutomaticWork);
  }
}

void Heap::FinishSweep() noexcept {
  phase_ = GcPhase::Idle;
  sweep_link_ = nullptr;
  const std::size_t doubled_live_size =
      allocated_bytes_ > std::numeric_limits<std::size_t>::max() / 2
          ? std::numeric_limits<std::size_t>::max()
          : allocated_bytes_ * 2;
  gc_threshold_ = std::max({minimum_gc_threshold_, doubled_live_size, std::size_t{1}});
}

void Heap::DestroyAll() noexcept {
  while (objects_ != nullptr) {
    HeapObject* object = objects_;
    objects_ = object->next;
    ReleaseRaw(*object);
  }
  object_count_ = 0;

  for (std::size_t index = 0; index < free_blocks_.size(); ++index) {
    HeapObject*& block = free_blocks_.at(index);
    while (block != nullptr) {
      HeapObject* next = block->next;
      committed_bytes_ -= kSizeClasses.at(index);
      std::destroy_at(block);
      ::operator delete(block);
      block = next;
    }
  }
}

}  // namespace vela::runtime
