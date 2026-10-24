#include "runtime/heap.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

namespace vela::runtime {
namespace {

constexpr std::size_t AlignUp(std::size_t value, std::size_t alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

ClassLayout::ClassLayout(std::uint32_t field_count, std::vector<std::uint32_t> reference_fields)
    : field_count_(field_count), reference_fields_(std::move(reference_fields)) {}

RootRange::RootRange(Heap& heap, Value* values, std::size_t count) noexcept
    : heap_(&heap), values_(values), count_(count) {
  heap_->RegisterRoot(*this);
}

RootRange::~RootRange() {
  if (heap_ != nullptr) {
    heap_->UnregisterRoot(*this);
  }
}

Heap::Heap(std::size_t gc_threshold) noexcept
    : gc_threshold_(std::max<std::size_t>(gc_threshold, 1)),
      minimum_gc_threshold_(std::max<std::size_t>(gc_threshold, 1)) {}

Heap::~Heap() {
  DestroyAll();
}

HeapObject* Heap::AllocateInstance(ClassLayout& layout,
                                   std::span<const Value> initial_fields) noexcept {
  if (initial_fields.size() > layout.field_count()) {
    return nullptr;
  }

  const std::size_t requested_size =
      AlignUp(sizeof(HeapObject) + sizeof(Value) * layout.field_count(), alignof(Value));
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
  std::copy(initial_fields.begin(), initial_fields.end(), fields);

  if (phase_ != GcPhase::Idle) {
    object->mark_bit = current_mark_bit_;
    if (phase_ == GcPhase::Mark) {
      object->color = 0;
      object->gray_next = gray_;
      gray_ = object;
    } else {
      object->color = 1;
      for (std::uint32_t index : layout.reference_fields()) {
        MarkValue(fields[index]);
      }
      DrainGray();
    }
  }

  objects_ = object;
  ++object_count_;
  return object;
}

Value* Heap::GetField(HeapObject& object, std::uint32_t index) noexcept {
  if (object.layout == nullptr || index >= object.layout->field_count()) {
    return nullptr;
  }
  return ObjectFields(&object) + index;
}

const Value* Heap::GetField(const HeapObject& object, std::uint32_t index) const noexcept {
  if (object.layout == nullptr || index >= object.layout->field_count()) {
    return nullptr;
  }
  return ObjectFields(&object) + index;
}

bool Heap::SetField(HeapObject& object, std::uint32_t index, Value value) noexcept {
  Value* field = GetField(object, index);
  if (field == nullptr) {
    return false;
  }
  *field = value;
  WriteBarrier(object, value);
  return true;
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
    for (std::size_t index = 0; index < root.count_; ++index) {
      MarkValue(root.values_[index]);
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
    for (std::size_t index = 0; index < root->count_; ++index) {
      MarkValue(root->values_[index]);
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
  if (object.layout == nullptr) {
    return;
  }
  const Value* fields = ObjectFields(&object);
  for (std::uint32_t index : object.layout->reference_fields()) {
    MarkValue(fields[index]);
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

HeapObject* Heap::AllocateRaw(std::size_t requested_size) noexcept {
  const std::size_t block_size = SelectBlockSize(requested_size);
  const std::size_t class_index = SizeClassIndex(block_size);
  void* memory = nullptr;

  if (class_index < free_blocks_.size() && free_blocks_[class_index] != nullptr) {
    FreeBlock* block = free_blocks_[class_index];
    free_blocks_[class_index] = block->next;
    memory = block;
  } else {
    memory = std::malloc(block_size);
    if (memory == nullptr) {
      return nullptr;
    }
    committed_bytes_ += block_size;
  }

  auto* object = static_cast<HeapObject*>(memory);
  std::construct_at(object);
  object->allocation_size = static_cast<std::uint32_t>(block_size);
  allocated_bytes_ += block_size;
  object->next = objects_;
  return object;
}

void Heap::ReleaseRaw(HeapObject& object) noexcept {
  const std::size_t block_size = object.allocation_size;
  allocated_bytes_ -= block_size;

  std::destroy_n(ObjectFields(&object), object.layout->field_count());
  std::destroy_at(&object);

  const std::size_t class_index = SizeClassIndex(block_size);
  if (class_index < free_blocks_.size()) {
    auto* block = reinterpret_cast<FreeBlock*>(&object);
    block->next = free_blocks_[class_index];
    free_blocks_[class_index] = block;
    return;
  }

  committed_bytes_ -= block_size;
  std::free(&object);
}

std::size_t Heap::SelectBlockSize(std::size_t requested_size) const noexcept {
  for (std::size_t block_size : kSizeClasses) {
    if (requested_size <= block_size) {
      return block_size;
    }
  }
  return requested_size;
}

std::size_t Heap::SizeClassIndex(std::size_t block_size) const noexcept {
  for (std::size_t index = 0; index < kSizeClasses.size(); ++index) {
    if (kSizeClasses[index] == block_size) {
      return index;
    }
  }
  return kSizeClasses.size();
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
  gc_threshold_ = std::max(minimum_gc_threshold_, std::max<std::size_t>(doubled_live_size, 1));
}

void Heap::DestroyAll() noexcept {
  while (objects_ != nullptr) {
    HeapObject* object = objects_;
    objects_ = object->next;
    ReleaseRaw(*object);
  }
  object_count_ = 0;

  for (std::size_t index = 0; index < free_blocks_.size(); ++index) {
    FreeBlock*& block = free_blocks_[index];
    while (block != nullptr) {
      FreeBlock* next = block->next;
      committed_bytes_ -= kSizeClasses[index];
      std::free(block);
      block = next;
    }
  }
}

}  // namespace vela::runtime
