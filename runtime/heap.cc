#include "runtime/heap.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include "base/execution_status.h"
#include "runtime/element_storage.h"
#include "runtime/exception.h"
#include "runtime/object.h"
#include "runtime/register_storage.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

template <typename T>
std::span<T> Slots(std::span<std::byte> bytes) noexcept {
  void* storage = bytes.data();
  return {static_cast<T*>(storage), bytes.size() / sizeof(T)};
}

template <typename T>
std::span<const T> Slots(std::span<const std::byte> bytes) noexcept {
  const void* storage = bytes.data();
  return {static_cast<const T*>(storage), bytes.size() / sizeof(T)};
}

std::unique_ptr<Future> AllocateFutureStorage(Future future) noexcept {
  return std::unique_ptr<Future>(new (std::nothrow) Future(std::move(future)));
}

void DestroyFutureStorage(Future* future) noexcept {
  if (future == nullptr) {
    return;
  }
  future->~Future();
  ::operator delete(future);
}

}  // namespace

Value LoadElementStorage(const ArrayLayout& layout,
                         std::span<const std::byte> storage,
                         std::size_t index) noexcept {
  if (layout.HasTaggedElements()) {
    return Slots<Value>(storage).subspan(index).front();
  }
  switch (layout.element.kind) {
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
  switch (layout.element.kind) {
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

constexpr std::size_t AlignUp(std::size_t value, std::size_t alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

ClassLayout::ClassLayout(std::uint32_t field_count, std::vector<std::uint32_t> reference_fields)
    : field_count_(field_count), reference_fields_(std::move(reference_fields)) {}

std::size_t ArrayLayout::element_size() const noexcept {
  if (HasTaggedElements()) {
    return sizeof(Value);
  }
  switch (element.kind) {
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

bool ClassLayout::IsImplementing(const InterfaceType& interface) const noexcept {
  return std::ranges::binary_search(interfaces_, &interface, std::less{});
}

void ClassLayout::BindCallableType(const TypeSignature& type) noexcept {
  assert(callable_.has_value());
  assert(callable_type_ == nullptr || callable_type_ == &type);
  callable_type_ = &type;
}

void ClassLayout::BindExpectedType(const TypeSignature& type) noexcept {
  assert(field_count_ == 1 && !callable_ && !is_enum_);
  assert(expected_type_ == nullptr || expected_type_ == &type);
  expected_type_ = &type;
}

void ClassLayout::BindCodeIdentity(const CodeIdentity& identity) noexcept {
  assert(callable_.has_value());
  assert(code_identity_ == nullptr || code_identity_ == &identity);
  code_identity_ = &identity;
}

std::optional<TypeConstraint> TypeSignature::FlattenedResult() const noexcept {
  if (!result || (*result).kind != ValueKind::Object ||
      (*result).object_kind != ObjectKind::Future) {
    return result;
  }
  auto payload = (*result).signature->result;
  if (payload && (*result).nullable && !(*payload).dynamic) {
    (*payload).nullable = true;
  }
  return payload;
}

const ClassLayout* Heap::ClassOf(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::Instance ? object.layout : nullptr;
}

HeapObject* Heap::AllocateFuture(Future future) noexcept {
  future.AttachHeap(*this);
  const std::size_t requested_size = AlignUp(sizeof(HeapObject), alignof(Value));
  MaybeCollect(requested_size);
  const auto allocation = AllocateRaw(requested_size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::Future;
  object->native_payload = AllocateFutureStorage(std::move(future)).release();
  if (object->native_payload == nullptr) {
    std::destroy_at(object);
    allocated_bytes_ -= allocation.size;
    ::operator delete(allocation.memory);
    return nullptr;
  }
  static_cast<Future*>(object->native_payload)->MakeHeapOwned();
  Publish(*object, allocation.size);
  return object;
}

Future* Heap::GetFuture(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::Future ? static_cast<Future*>(object.native_payload) : nullptr;
}

HeapObject* Heap::AllocateCompleter(Future future) noexcept {
  auto* object = AllocateFuture(std::move(future));
  if (object != nullptr) {
    object->kind = ObjectKind::Completer;
  }
  return object;
}

Future* Heap::GetCompleter(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::Completer ? static_cast<Future*>(object.native_payload)
                                              : nullptr;
}

HeapObject* Heap::AllocateTimer(Timer timer) noexcept {
  MaybeCollect(sizeof(HeapObject));
  const auto allocation = AllocateRaw(sizeof(HeapObject));
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::Timer;
  object->native_payload = std::make_unique<Timer>(std::move(timer)).release();
  Publish(*object, allocation.size);
  return object;
}

Timer* Heap::GetTimer(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::Timer ? static_cast<Timer*>(object.native_payload) : nullptr;
}

HeapObject* Heap::AllocateCancellationToken(CancellationToken token) noexcept {
  MaybeCollect(sizeof(HeapObject));
  const auto allocation = AllocateRaw(sizeof(HeapObject));
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::CancellationToken;
  object->native_payload = std::make_unique<CancellationToken>(std::move(token)).release();
  Publish(*object, allocation.size);
  return object;
}

CancellationToken* Heap::GetCancellationToken(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::CancellationToken
             ? static_cast<CancellationToken*>(object.native_payload)
             : nullptr;
}

TypeConstraint Heap::TypeOf(Value value) noexcept {
  TypeConstraint type{.kind = value.kind()};
  if (value.IsNull()) {
    type.nullable = true;
  } else if (value.IsObject()) {
    const auto& object = *value.AsObject();
    if (const auto* bound = RuntimeTypeOf(object)) {
      return *bound;
    }
    type.object_kind = KindOf(object);
    type.instance = ClassOf(object);
    type.array = ArrayTypeOf(object);
    type.collection = CollectionTypeOf(object);
    if (object.kind == ObjectKind::Expected) {
      type.signature = object.layout->expected_type();
    }
    if (type.instance != nullptr && type.instance->callable()) {
      type.callable = true;
      type.signature = type.instance->callable_type();
      type.instance = nullptr;
    }
  }
  return type;
}

bool TypeConstraint::IsSubtypeOf(const TypeConstraint& target) const noexcept {
  auto actual = *this;
  auto expected = target;
  while (true) {
    if (expected.dynamic) {
      return true;
    }
    if (actual.dynamic || (actual.nullable && !expected.nullable)) {
      return false;
    }
    actual.nullable = false;
    expected.nullable = false;
    if (actual == expected) {
      return true;
    }
    if ((expected.interface != nullptr) && actual.instance != nullptr &&
        actual.instance->IsImplementing(*expected.interface)) {
      return true;
    }
    if (actual.kind != ValueKind::Object || expected.kind != ValueKind::Object ||
        actual.object_kind != ObjectKind::Future || expected.object_kind != ObjectKind::Future ||
        actual.signature == nullptr || expected.signature == nullptr) {
      return false;
    }
    const auto& actual_result = actual.signature->result;
    const auto& expected_result = expected.signature->result;
    if (!actual_result || !expected_result) {
      return !actual_result && !expected_result;
    }
    actual = *actual_result;
    expected = *expected_result;
  }
}

bool Heap::IsThrowable(Value value) noexcept {
  const auto* layout = value.IsObject() ? ClassOf(*value.AsObject()) : nullptr;
  return layout != nullptr && !layout->IsEnum() && !layout->callable();
}

bool Heap::IsValueOfType(Value value, const TypeConstraint& type) noexcept {
  if (type.dynamic) {
    return true;
  }
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
  if ((type.interface != nullptr) &&
      (object->layout == nullptr || !object->layout->IsImplementing(*type.interface))) {
    return false;
  }
  if ((type.signature != nullptr) && !type.callable && !TypeOf(value).IsSubtypeOf(type)) {
    return false;
  }
  return !type.callable ||
         (object->layout != nullptr && object->layout->callable().has_value() &&
          (type.signature == nullptr || object->layout->callable_type() == type.signature));
}

std::expected<void, ExecutionStatus> Heap::BindRuntimeType(HeapObject& object,
                                                           const ArrayLayout& type) noexcept {
  if ((object.kind != ObjectKind::Future && object.kind != ObjectKind::Completer) ||
      object.kind != type.element.object_kind) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (object.indexed_layout != nullptr) {
    return object.array_layout()->element.IsSubtypeOf(type.element)
               ? std::expected<void, ExecutionStatus>{}
               : std::unexpected(ExecutionStatus::InvalidCast);
  }
  object.indexed_layout = &type;
  return {};
}

HeapObject* Heap::AllocateInstance(const ClassLayout& layout,
                                   std::span<const Value> initial_fields) noexcept {
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

HeapObject* Heap::AllocateExpected(ClassLayout& layout,
                                   Value payload,
                                   ExpectedAlternative alternative) noexcept {
  auto* object = AllocateInstance(layout, std::span(&payload, 1));
  if (object != nullptr) {
    object->kind = ObjectKind::Expected;
    object->length = static_cast<std::uint32_t>(alternative);
  }
  return object;
}

std::expected<ExpectedAlternative, ExpectedAccessError> Heap::ExpectedAlternativeOf(
    const HeapObject& object) noexcept {
  if (object.kind != ObjectKind::Expected) {
    return std::unexpected(ExpectedAccessError::NotExpected);
  }
  return static_cast<ExpectedAlternative>(object.length);
}

std::expected<Value, ExpectedAccessError> Heap::ReadExpectedValue(
    const HeapObject& object,
    ExpectedAlternative alternative) noexcept {
  const auto active = ExpectedAlternativeOf(object);
  if (!active) {
    return std::unexpected(active.error());
  }
  if (*active != alternative) {
    return std::unexpected(ExpectedAccessError::InactiveAlternative);
  }
  return *GetField(object, 0);
}

void Heap::Publish(HeapObject& object, std::uint32_t allocation_size) noexcept {
  object.mark_bit = current_mark_bit_;
  object.allocation_size = allocation_size;
  object.next = objects_;
  if (phase_ == GcPhase::Mark && object.kind != ObjectKind::String &&
      object.kind != ObjectKind::MutableString && object.kind != ObjectKind::ByteArray) {
    object.gray_next = gray_;
    gray_ = &object;
  } else if (phase_ == GcPhase::Sweep) {
    ProcessGrayObject(object);
    DrainGray();
  }
  objects_ = &object;
  ++object_count_;
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
  std::erase_if(future_roots_, [](const auto& root) { return root.IsExpired(); });
  for (const auto& root : future_roots_) {
    root.Trace(*this);
  }
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
  if (object.kind == ObjectKind::String || object.kind == ObjectKind::MutableString ||
      object.kind == ObjectKind::ByteArray) {
    return;
  }
  object.gray_next = gray_;
  gray_ = &object;
}

void Heap::ProcessGrayObject(HeapObject& object) noexcept {
  if (object.kind == ObjectKind::RegExp || object.kind == ObjectKind::RegExpMatch) {
    MarkRegExp(object);
    return;
  }
  if (object.kind == ObjectKind::ByteData) {
    MarkByteData(object);
    return;
  }
  if (object.kind == ObjectKind::ValueSlots) {
    for (const auto value : ValueSlots(object)) {
      MarkValue(value);
    }
    return;
  }
  if (object.kind == ObjectKind::Future || object.kind == ObjectKind::Completer) {
    if (static_cast<Future*>(object.native_payload) != nullptr) {
      static_cast<Future*>(object.native_payload)->Trace(*this);
    }
    return;
  }
  if (object.kind == ObjectKind::Exception) {
    MarkObject(*ExceptionData::From(object).payload);
    return;
  }
  if (object.kind == ObjectKind::Collection) {
    MarkCollection(object);
    return;
  }
  if (object.kind == ObjectKind::Array) {
    MarkArray(object);
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

Heap::Allocation Heap::AllocateRaw(std::size_t requested_size) noexcept {
  const auto [block_size, class_index] = SelectBlockClass(requested_size);
  void* memory = nullptr;

  if (class_index < free_blocks_.size()) {
    auto& free_block = std::span(free_blocks_).subspan(class_index).front();
    if (free_block != nullptr) {
      HeapObject* block = free_block;
      free_block = block->next;
      memory = block;
      std::destroy_at(block);
    }
  }
  if (memory == nullptr) {
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

  if (object.kind == ObjectKind::RegExp) {
    ReleaseRegExp(object);
  } else if (object.layout != nullptr) {
    std::destroy_n(ObjectFields(&object), object.layout->field_count());
  } else if (object.kind == ObjectKind::MutableString) {
    ReleaseMutableString(object);
  } else if (object.kind == ObjectKind::Collection) {
    ReleaseCollection(object);
  } else if (object.kind == ObjectKind::Array) {
    ReleaseArray(object);
  } else if (object.kind == ObjectKind::Future || object.kind == ObjectKind::Completer) {
    DestroyFutureStorage(static_cast<Future*>(object.native_payload));
    object.native_payload = nullptr;
  } else if (object.kind == ObjectKind::Exception) {
    const std::unique_ptr<FutureDiagnostic> diagnostic(
        ExceptionData::From(object).async_diagnostic);
  } else if (object.kind == ObjectKind::Timer) {
    const std::unique_ptr<Timer> owned(static_cast<Timer*>(object.native_payload));
  } else if (object.kind == ObjectKind::CancellationToken) {
    const std::unique_ptr<CancellationToken> owned(
        static_cast<CancellationToken*>(object.native_payload));
  } else if (object.kind == ObjectKind::ValueSlots) {
    const auto bytes = object.length * sizeof(Value);
    allocated_bytes_ -= bytes;
    committed_bytes_ -= bytes;
    std::destroy_n(static_cast<Value*>(object.native_payload), object.length);
    ::operator delete(object.native_payload);
  }
  void* memory = &object;
  // StringObject 仅含可平凡析构的对象头；复用或释放存储即结束其生命周期。
  std::destroy_at(&object);
  const auto class_index = SelectBlockClass(block_size).second;
  if (class_index < free_blocks_.size()) {
    auto* block = std::construct_at(static_cast<HeapObject*>(memory));
    auto& free_block = std::span(free_blocks_).subspan(class_index).front();
    block->next = free_block;
    free_block = block;
    return;
  }

  committed_bytes_ -= block_size;
  ::operator delete(memory);
}

std::pair<std::size_t, std::size_t> Heap::SelectBlockClass(std::size_t requested_size) noexcept {
  for (std::size_t index = 0; index < kSizeClasses.size(); ++index) {
    const auto block_size = std::span(kSizeClasses).subspan(index).front();
    if (requested_size <= block_size) {
      return {block_size, index};
    }
  }
  return {requested_size, kSizeClasses.size()};
}

bool Heap::ShouldCollect(std::size_t next_size) const noexcept {
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
    HeapObject*& block = std::span(free_blocks_).subspan(index).front();
    while (block != nullptr) {
      HeapObject* next = block->next;
      committed_bytes_ -= kSizeClasses.at(index);
      std::destroy_at(block);
      ::operator delete(block);
      block = next;
    }
  }
  allocated_bytes_ = 0;
  committed_bytes_ = 0;
}

}  // namespace vela::runtime
