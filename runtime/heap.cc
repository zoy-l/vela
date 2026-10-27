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
#include "runtime/exception.h"
#include "runtime/object.h"
#include "runtime/register_storage.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
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
    : gc_threshold_(std::max<std::size_t>(gc_threshold, 1)), minimum_gc_threshold_(gc_threshold_) {}

Heap::~Heap() {
  DestroyAll();
}

HeapObject* Heap::AllocateFuture(Future future) noexcept {
  future.AttachHeap(*this);
  return AllocateNativeObject(ObjectKind::Future, std::move(future));
}

Future* Heap::GetFuture(const HeapObject& object) noexcept {
  return NativeOf<ObjectKind::Future, Future>(object);
}

HeapObject* Heap::AllocateCompleter(Future future) noexcept {
  auto* object = AllocateFuture(std::move(future));
  if (object != nullptr) {
    object->kind = ObjectKind::Completer;
  }
  return object;
}

Future* Heap::GetCompleter(const HeapObject& object) noexcept {
  return NativeOf<ObjectKind::Completer, Future>(object);
}

HeapObject* Heap::AllocateTimer(Timer timer) noexcept {
  return AllocateNativeObject(ObjectKind::Timer, std::move(timer));
}

Timer* Heap::GetTimer(const HeapObject& object) noexcept {
  return NativeOf<ObjectKind::Timer, Timer>(object);
}

HeapObject* Heap::AllocateCancellationToken(CancellationToken token) noexcept {
  return AllocateNativeObject(ObjectKind::CancellationToken, std::move(token));
}

CancellationToken* Heap::GetCancellationToken(const HeapObject& object) noexcept {
  return NativeOf<ObjectKind::CancellationToken, CancellationToken>(object);
}

HeapObject* Heap::AllocateInstance(const ClassLayout& layout,
                                   std::span<const Value> initial_fields) noexcept {
  if (initial_fields.size() > layout.field_count()) {
    return nullptr;
  }

  const std::size_t requested_size = sizeof(HeapObject) + (sizeof(Value) * layout.field_count());
  if (requested_size > std::numeric_limits<std::uint32_t>::max()) {
    return nullptr;
  }
  return AllocateObject(ObjectKind::Instance, requested_size,
                        [&layout, initial_fields](HeapObject& object) {
                          object.layout = &layout;
                          auto* fields = ObjectFields(&object);
                          std::uninitialized_fill_n(fields, layout.field_count(), Value::Null());
                          std::ranges::copy(initial_fields, fields);
                        });
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

void Heap::Publish(HeapObject& object, const Allocation& allocation) noexcept {
  object.mark_bit = current_mark_bit_;
  object.allocation_size = allocation.size;
  object.size_class = allocation.size_class;
  object.next = objects_;
  if (phase_ == GcPhase::Mark && !IsLeafObject(object.kind)) {
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
    for (auto* string : std::span(static_cast<StringObject* const*>(root.data_), root.count_)) {
      MarkReference(string);
    }
  }
}

void Heap::MarkReference(HeapObject* object) noexcept {
  if (object != nullptr) {
    MarkObject(*object);
  }
}

void Heap::MarkReference(StringObject* string) noexcept {
  if (string != nullptr) {
    MarkObject(string->header_);
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
  if (IsLeafObject(object.kind)) {
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
  return {
      .memory = memory,
      .size = static_cast<std::uint32_t>(block_size),
      .size_class = static_cast<std::uint8_t>(class_index),
  };
}

void Heap::ReleaseRaw(HeapObject& object) noexcept {
  const std::size_t block_size = object.allocation_size;
  const auto class_index = object.size_class;
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
    const std::unique_ptr<Future> owned(static_cast<Future*>(object.native_payload));
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
