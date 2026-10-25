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
#include "runtime/element_storage.h"
#include "runtime/exception.h"
#include "runtime/object.h"
#include "runtime/register_storage.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

template <typename T>
auto Slots(std::span<std::byte> bytes) noexcept -> std::span<T> {
  void* storage = bytes.data();
  return {static_cast<T*>(storage), bytes.size() / sizeof(T)};
}

template <typename T>
auto Slots(std::span<const std::byte> bytes) noexcept -> std::span<const T> {
  const void* storage = bytes.data();
  return {static_cast<const T*>(storage), bytes.size() / sizeof(T)};
}

}  // namespace

auto ArrayLayout::element_type() const noexcept -> TypeConstraint {
  return {
      .kind = element_kind,
      .object_kind = element_object_kind,
      .instance = element_class,
      .array = element_array,
      .collection = element_collection,
      .interface = element_interface,
      .nullable = nullable_elements,
      .callable = callable_elements,
  };
}

auto LoadElementStorage(const ArrayLayout& layout,
                        std::span<const std::byte> storage,
                        std::size_t index) noexcept -> Value {
  if (layout.HasTaggedElements()) {
    return Slots<Value>(storage).subspan(index).front();
  }
  switch (layout.element_kind) {
    case ValueKind::Boolean:
      return Value::Boolean(Slots<bool>(storage).subspan(index).front());
    case ValueKind::Integer:
      return Value::Integer(Slots<std::int64_t>(storage).subspan(index).front());
    case ValueKind::Double:
      return Value::Double(Slots<double>(storage).subspan(index).front());
    case ValueKind::String: {
      auto* value = Slots<StringObject*>(storage).subspan(index).front();
      return value == nullptr ? Value::Null() : Value::String(value);
    }
    case ValueKind::Object: {
      auto* value = Slots<HeapObject*>(storage).subspan(index).front();
      return value == nullptr ? Value::Null() : Value::Object(value);
    }
    case ValueKind::Null:
      std::unreachable();
  }
  std::unreachable();
}

void StoreElementStorage(const ArrayLayout& layout,
                         std::span<std::byte> storage,
                         std::size_t index,
                         Value value) noexcept {
  if (layout.HasTaggedElements()) {
    Slots<Value>(storage).subspan(index).front() = value;
    return;
  }
  switch (layout.element_kind) {
    case ValueKind::Boolean:
      Slots<bool>(storage).subspan(index).front() = value.AsBoolean();
      return;
    case ValueKind::Integer:
      Slots<std::int64_t>(storage).subspan(index).front() = value.AsInteger();
      return;
    case ValueKind::Double:
      Slots<double>(storage).subspan(index).front() = value.AsDouble();
      return;
    case ValueKind::String:
      Slots<StringObject*>(storage).subspan(index).front() =
          value.IsNull() ? nullptr : value.AsString();
      return;
    case ValueKind::Object:
      Slots<HeapObject*>(storage).subspan(index).front() =
          value.IsNull() ? nullptr : value.AsObject();
      return;
    case ValueKind::Null:
      std::unreachable();
  }
  std::unreachable();
}

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

ClassLayout::ClassLayout(std::uint32_t field_count, std::vector<std::uint32_t> reference_fields)
    : field_count_(field_count), reference_fields_(std::move(reference_fields)) {}

auto ArrayLayout::element_size() const noexcept -> std::size_t {
  if (HasTaggedElements()) {
    return sizeof(Value);
  }
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

RootRange::RootRange(Heap& heap, const Value* values, std::size_t count) noexcept {
  Attach(heap, values, count);
}

RootRange::RootRange(Heap& heap, std::span<StringObject* const> strings) noexcept
    : heap_(&heap),
      data_(static_cast<const void*>(strings.data())),
      count_(strings.size()),
      kind_(Kind::Strings) {
  heap.RegisterRoot(*this);
}

RootRange::~RootRange() {
  Detach();
}

void RootRange::Attach(Heap& heap, const Value* values, std::size_t count) noexcept {
  Detach();
  heap_ = &heap;
  data_ = values;
  count_ = count;
  kind_ = Kind::Values;
  heap_->RegisterRoot(*this);
}

void RootRange::Detach() noexcept {
  if (heap_ == nullptr) {
    return;
  }
  vela::runtime::Heap::UnregisterRoot(*this);
  heap_ = nullptr;
}

void RootRange::Attach(Heap& heap, RegisterView registers) noexcept {
  Detach();
  heap_ = &heap;
  data_ = registers.payloads().data();
  register_kinds_ = registers.kinds().data();
  count_ = registers.size();
  kind_ = Kind::Registers;
  heap.RegisterRoot(*this);
}

Heap::Heap(std::size_t gc_threshold) noexcept
    : gc_threshold_(std::max<std::size_t>(gc_threshold, 1)),
      minimum_gc_threshold_(std::max<std::size_t>(gc_threshold, 1)) {}

Heap::~Heap() {
  DestroyAll();
}

auto ClassLayout::IsImplementing(std::uint32_t interface) const noexcept -> bool {
  return std::ranges::binary_search(interfaces_, interface);
}

auto Heap::ClassOf(const HeapObject& object) noexcept -> const ClassLayout* {
  return object.kind == ObjectKind::Instance ? object.layout : nullptr;
}

auto Heap::IsValueOfType(Value value, const TypeConstraint& type) noexcept -> bool {
  if (value.IsNull()) {
    return type.nullable;
  }
  if (value.kind() != type.kind) {
    return false;
  }
  if (!value.IsObject()) {
    return true;
  }
  const auto* object = value.AsObject();
  if (object == nullptr || object->kind != type.object_kind ||
      (type.instance != nullptr && object->layout != type.instance) ||
      (type.array != nullptr && object->array_layout() != type.array) ||
      (type.collection != nullptr && object->collection_layout() != type.collection)) {
    return false;
  }
  if (type.interface &&
      (object->layout == nullptr || !object->layout->IsImplementing(*type.interface))) {
    return false;
  }
  return !type.callable || (object->layout != nullptr && object->layout->callable().has_value());
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

  const auto allocation = AllocateRaw(requested_size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->layout = &layout;

  Value* fields = ObjectFields(object);
  std::uninitialized_fill_n(fields, layout.field_count(), Value::Null());
  std::ranges::copy(initial_fields, fields);

  Publish(*object, allocation.size);
  return object;
}

auto Heap::AllocateExpected(ClassLayout& layout,
                            Value payload,
                            ExpectedAlternative alternative) noexcept -> HeapObject* {
  auto* object = AllocateInstance(layout, std::span(&payload, 1));
  if (object != nullptr) {
    object->kind = ObjectKind::Expected;
    object->length = static_cast<std::uint32_t>(alternative);
  }
  return object;
}

auto Heap::ExpectedAlternativeOf(const HeapObject& object) noexcept
    -> std::expected<ExpectedAlternative, ExpectedAccessError> {
  if (object.kind != ObjectKind::Expected) {
    return std::unexpected(ExpectedAccessError::NotExpected);
  }
  return static_cast<ExpectedAlternative>(object.length);
}

auto Heap::ReadExpectedValue(const HeapObject& object, ExpectedAlternative alternative) noexcept
    -> std::expected<Value, ExpectedAccessError> {
  const auto active = ExpectedAlternativeOf(object);
  if (!active) {
    return std::unexpected(active.error());
  }
  if (*active != alternative) {
    return std::unexpected(ExpectedAccessError::InactiveAlternative);
  }
  return *GetField(object, 0);
}

auto Heap::AllocateArray(const ArrayLayout& layout, std::size_t length) noexcept -> HeapObject* {
  const std::size_t element_size = layout.element_size();
  if (element_size == 0 ||
      length > (std::numeric_limits<std::uint32_t>::max() - sizeof(HeapObject)) / element_size) {
    return nullptr;
  }
  const std::size_t requested_size = sizeof(HeapObject) + (length * element_size);
  MaybeCollect(requested_size);
  const auto allocation = AllocateRaw(requested_size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::Array;
  object->indexed_layout = &layout;
  object->length = static_cast<std::uint32_t>(length);
  // payload 与对象头单次分配；按原生元素类型建立对象生命周期。
  if (layout.HasTaggedElements()) {
    std::uninitialized_value_construct_n(ArrayElements<Value>(*object, length).data(), length);
  } else {
    InitializeArray(*object, layout.element_kind, length);
  }
  Publish(*object, allocation.size);
  return object;
}

void Heap::Publish(HeapObject& object, std::uint32_t allocation_size) noexcept {
  object.mark_bit = current_mark_bit_;
  object.allocation_size = allocation_size;
  object.next = objects_;
  if (phase_ == GcPhase::Mark && object.kind != ObjectKind::String &&
      object.kind != ObjectKind::StringBuilder) {
    object.gray_next = gray_;
    gray_ = &object;
  } else if (phase_ == GcPhase::Sweep) {
    ProcessGrayObject(object);
    DrainGray();
  }
  objects_ = &object;
  ++object_count_;
}

auto Heap::ArrayLength(const HeapObject& array) noexcept
    -> std::expected<std::uint32_t, ArrayAccessError> {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  return array.length;
}

auto Heap::IntegerElements(HeapObject& array) noexcept
    -> std::expected<std::span<std::int64_t>, ArrayAccessError> {
  if (array.kind != ObjectKind::Array || array.array_layout()->element_kind != ValueKind::Integer ||
      array.array_layout()->HasTaggedElements()) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  return ArrayElements<std::int64_t>(array, array.length);
}

auto Heap::GetElement(const HeapObject& object, std::uint32_t index) noexcept
    -> std::expected<Value, ArrayAccessError> {
  if (object.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  const auto length = object.length;
  if (index >= length) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  const auto& layout = *object.array_layout();
  const void* data = std::span(&object, 1).last(0).data();
  const auto bytes =
      std::span(static_cast<const std::byte*>(data), object.allocation_size - sizeof(HeapObject));
  return LoadElementStorage(layout, bytes, index);
}

auto Heap::SetElement(HeapObject& object, std::uint32_t index, Value value) noexcept
    -> std::expected<void, ArrayAccessError> {
  if (object.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  const auto length = object.length;
  if (index >= length) {
    return std::unexpected(ArrayAccessError::OutOfBounds);
  }
  const auto& layout = *object.array_layout();
  if (!IsValueOfType(value, layout.element_type())) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  void* data = std::span(&object, 1).last(0).data();
  const auto bytes =
      std::span(static_cast<std::byte*>(data), object.allocation_size - sizeof(HeapObject));
  StoreElementStorage(layout, bytes, index, value);
  if (value.IsObject() || value.IsString()) {
    WriteBarrier(object, value);
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
      // VM 可在增量步骤之间改写根槽位；进入清扫前重新捕获当前根。
      MarkRoots();
      if (gray_ != nullptr) {
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
    MarkRoot(root);
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
    MarkRoot(*root);
  }
}

void Heap::MarkRoot(const RootRange& root) noexcept {
  if (root.kind_ == RootRange::Kind::Registers) {
    const auto payloads = std::span(static_cast<const RegisterPayload*>(root.data_), root.count_);
    const auto kinds = std::span(root.register_kinds_, root.count_);
    for (std::size_t index = 0; index < root.count_; ++index) {
      const auto kind = kinds.subspan(index).front();
      const auto& payload = payloads.subspan(index).front();
      if (kind == ValueKind::Object) {
        if (auto* object = payload.Read<HeapObject*>(); object != nullptr) {
          MarkObject(*object);
        }
      } else if (kind == ValueKind::String) {
        if (auto* string = payload.Read<StringObject*>(); string != nullptr) {
          MarkObject(string->header_);
        }
      }
    }
    return;
  }
  if (root.kind_ == RootRange::Kind::Values) {
    for (const Value value : std::span(static_cast<const Value*>(root.data_), root.count_)) {
      MarkValue(value);
    }
  } else {
    for (StringObject* string :
         std::span(static_cast<StringObject* const*>(root.data_), root.count_)) {
      if (string != nullptr) {
        MarkObject(string->header_);
      }
    }
  }
}

void Heap::MarkValue(Value value) noexcept {
  if (value.IsObject() && value.AsObject() != nullptr) {
    MarkObject(*value.AsObject());
  } else if (value.IsString() && value.AsString() != nullptr) {
    MarkObject(value.AsString()->header_);
  }
}

void Heap::MarkObject(HeapObject& object) noexcept {
  if (object.mark_bit == current_mark_bit_) {
    return;
  }
  object.mark_bit = current_mark_bit_;
  if (object.kind == ObjectKind::String || object.kind == ObjectKind::StringBuilder) {
    return;
  }
  object.gray_next = gray_;
  gray_ = &object;
}

void Heap::ProcessGrayObject(HeapObject& object) noexcept {
  if (object.kind == ObjectKind::Exception) {
    MarkObject(*ExceptionData::From(object).payload);
    return;
  }
  if (object.kind == ObjectKind::Collection) {
    MarkCollection(object);
    return;
  }
  if (object.kind == ObjectKind::Array) {
    if (object.array_layout()->element_kind == ValueKind::Object) {
      for (HeapObject* child : ArrayElements<HeapObject*>(object, object.length)) {
        if (child != nullptr) {
          MarkObject(*child);
        }
      }
    } else if (object.array_layout()->element_kind == ValueKind::String) {
      for (StringObject* string : ArrayElements<StringObject*>(object, object.length)) {
        if (string != nullptr) {
          MarkObject(string->header_);
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

auto Heap::AllocateRaw(std::size_t requested_size) noexcept -> Allocation {
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
      return {};
    }
    committed_bytes_ += block_size;
  }

  allocated_bytes_ += block_size;
  return {.memory = memory, .size = static_cast<std::uint32_t>(block_size)};
}

void Heap::ReleaseRaw(HeapObject& object) noexcept {
  const std::size_t block_size = object.allocation_size;
  allocated_bytes_ -= block_size;

  if (object.layout != nullptr) {
    std::destroy_n(ObjectFields(&object), object.layout->field_count());
  } else if (object.kind == ObjectKind::StringBuilder) {
    ReleaseStringBuilder(object);
  } else if (object.kind == ObjectKind::Collection) {
    ReleaseCollection(object);
  }
  void* memory = &object;
  // StringObject 仅含可平凡析构的对象头；复用或释放存储即结束其生命周期。
  std::destroy_at(&object);
  const std::size_t class_index = SizeClassIndex(block_size);
  if (class_index < free_blocks_.size()) {
    auto* block = std::construct_at(static_cast<HeapObject*>(memory));
    block->next = free_blocks_.at(class_index);
    free_blocks_.at(class_index) = block;
    return;
  }

  committed_bytes_ -= block_size;
  ::operator delete(memory);
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
