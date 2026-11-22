#include "runtime/heap/heap.h"
#include "runtime/reflection/reflection_handle.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/exceptions/exception.h"
#include "runtime/heap/block_page_memory.h"
#include "runtime/heap/field_storage.h"
#include "runtime/heap/instance_storage.h"
#include "runtime/heap/intrusive_link_internal.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

// PCRE2 和 yyjson 的释放回调不携带长度，大小必须随分配块保存。
struct alignas(std::max_align_t) NativeHeader final {
  std::size_t size;
};

// 同类对象集中在独立页，回收时整页归还，避免系统小对象分配器保留历史碎片。
// 页对齐使对象可以在不扩充其头部的情况下定位所属页；大对象仍独立分配。
constexpr auto kBlockPageBytes = internal::kHeapBlockPageBytes;

}  // namespace

RootRange::RootRange(Heap& heap,
                     std::span<StringObject* const> strings,
                     std::optional<ExternalResourceKind> resource_kind) noexcept
    : heap_(&heap),
      data_(static_cast<const void*>(strings.data())),
      count_(strings.size()),
      kind_(Kind::Strings),
      resource_kind_(resource_kind) {
  heap.RegisterRoot(*this);
}

RootRange::RootRange(Heap& heap,
                     std::span<const TypeConstraint> types,
                     std::optional<ExternalResourceKind> resource_kind) noexcept
    : heap_(&heap),
      data_(types.data()),
      count_(types.size()),
      kind_(Kind::Types),
      resource_kind_(resource_kind) {
  heap.RegisterRoot(*this);
}

void RootRange::Attach(Heap& heap, const Value* values, std::size_t count) noexcept {
  Detach();
  heap_ = &heap;
  data_ = values;
  count_ = count;
  kind_ = Kind::Values;
  heap_->RegisterRoot(*this);
}

void RootRange::Attach(Heap& heap, const InstanceStorage& fields) noexcept {
  Detach();
  heap_ = &heap;
  data_ = &fields;
  count_ = fields.count;
  kind_ = Kind::InstanceFields;
  heap_->RegisterRoot(*this);
}

Heap::Heap(std::size_t gc_threshold, GcScheduling scheduling) noexcept
    : gc_threshold_(std::max<std::size_t>(gc_threshold, 1)),
      minimum_gc_threshold_(gc_threshold_),
      gc_scheduling_(scheduling),
      host_index_(*this) {}

Heap::~Heap() {
  DestroyAll();
}

Heap::Heap(Isolate& isolate, std::size_t gc_threshold, GcScheduling scheduling) noexcept
    : Heap(gc_threshold, scheduling) {
  isolate_ = &isolate;
}

void* Heap::AllocateExternal(std::size_t bytes, std::size_t replaced_bytes) noexcept {
  if (!CanAllocate(bytes - replaced_bytes)) {
    return nullptr;
  }
  const auto class_index = SelectBlockClass(bytes).second;
  void* memory = nullptr;
  if (class_index < available_pages_.size()) {
    memory = AllocatePooledBlock(class_index);
  } else {
    memory = ::operator new(bytes, std::nothrow);
    if (memory != nullptr) {
      committed_bytes_ += bytes;
    }
  }
  if (memory != nullptr) {
    RecordAllocation(bytes);
  }
  return memory;
}

void Heap::RecordExternalAllocation(std::size_t bytes) noexcept {
  committed_bytes_ += bytes;
  RecordAllocation(bytes);
}

void Heap::RecordAllocation(std::size_t bytes) noexcept {
  allocated_bytes_ += bytes;
  allocation_statistics_.bytes += bytes;
  ++allocation_statistics_.count;
  allocation_statistics_.peak_live_bytes =
      std::max(allocation_statistics_.peak_live_bytes, allocated_bytes_);
}

std::expected<void, ExecutionStatus> Heap::ReserveHostMemory(std::size_t bytes) noexcept {
  MaybeCollect(bytes);
  if (!CanAllocate(bytes) || bytes > std::numeric_limits<std::size_t>::max() - committed_bytes_) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  RecordExternalAllocation(bytes);
  return {};
}

void Heap::ReleaseHostMemory(std::size_t bytes) noexcept {
  allocated_bytes_ -= bytes;
  committed_bytes_ -= bytes;
}

void* Heap::AllocateNative(std::size_t size) noexcept {
  if (size > std::numeric_limits<std::size_t>::max() - sizeof(NativeHeader)) {
    return nullptr;
  }
  const auto total = size + sizeof(NativeHeader);
  MaybeCollect(total);
  auto* memory = AllocateExternal(total);
  if (memory == nullptr) {
    return nullptr;
  }
  auto* header = std::construct_at(static_cast<NativeHeader*>(memory), total);
  return StorageAfter<std::byte>(*header);
}

void Heap::FreeNative(void* pointer) noexcept {
  if (pointer == nullptr) {
    return;
  }
  auto* header = std::prev(static_cast<NativeHeader*>(pointer));
  const auto bytes = header->size;
  std::destroy_at(header);
  ReleaseExternalStorage(header, bytes);
}

void Heap::ReleaseExternalStorage(void* storage, std::size_t bytes) noexcept {
  if (storage == nullptr) {
    return;
  }
  allocated_bytes_ -= bytes;
  const auto class_index = SelectBlockClass(bytes).second;
  if (class_index < available_pages_.size()) {
    ReleasePooledBlock(storage, class_index);
  } else {
    committed_bytes_ -= bytes;
    ::operator delete(storage);
  }
}

HeapObject* Heap::AllocateFuture(Future future) noexcept {
  return AllocateFutureObject(ObjectKind::Future, std::move(future));
}

HeapObject* Heap::AllocateFutureObject(ObjectKind kind, Future future) noexcept {
  future.AttachHeap(*this);
  return AllocateObject(kind, sizeof(HeapObject) + sizeof(Future), [&future](HeapObject& object) {
    std::construct_at(StorageAfter<Future>(object), future.Internal());
  });
}

Future* Heap::GetFuture(HeapObject& object) noexcept {
  return object.kind == ObjectKind::Future ? StorageAfter<Future>(object) : nullptr;
}

HeapObject* Heap::AllocateCompleter(Future future) noexcept {
  return AllocateFutureObject(ObjectKind::Completer, std::move(future));
}

Future* Heap::GetCompleter(HeapObject& object) noexcept {
  return object.kind == ObjectKind::Completer ? StorageAfter<Future>(object) : nullptr;
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

  if (layout.storage() == ClassStorage::Reloadable) {
    auto* storage = AllocateInstanceStorage(layout.field_count());
    if (storage == nullptr) {
      return nullptr;
    }
    // AllocateRaw 不触发 GC；先前传入的 initial_fields 不会在块准备后失根。
    const auto allocation = AllocateRaw(sizeof(HeapObject));
    if (allocation.memory == nullptr) {
      ReleaseInstanceStorage(*storage);
      return nullptr;
    }
    auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
    object->layout = &layout;
    object->native_payload = storage;
    const auto fields = storage->fields();
    for (std::uint32_t index = 0; index < initial_fields.size(); ++index) {
      FieldStorage::Store(fields, storage->count, index, initial_fields.subspan(index).front());
    }
    std::ranges::fill(storage->states().first(initial_fields.size()), FieldState::Ready);
    storage->unready -= static_cast<std::uint32_t>(initial_fields.size());
    RegisterInstance(*object, *storage);
    Publish(*object, allocation);
    return object;
  }
  const std::size_t requested_size = sizeof(HeapObject) + FieldStorage::Size(layout.field_count());
  if (requested_size > std::numeric_limits<std::uint32_t>::max()) {
    return nullptr;
  }
  return AllocateObject(ObjectKind::Instance, requested_size,
                        [&layout, initial_fields](HeapObject& object) {
                          object.layout = &layout;
                          object.auxiliary_length = layout.field_count();
                          const auto fields = ObjectFieldStorage(object);
                          static_assert(static_cast<std::uint8_t>(ValueKind::Null) == 0);
                          std::uninitialized_fill_n(fields.data(), fields.size(), std::byte{0});
                          for (std::uint32_t index = 0; index < initial_fields.size(); ++index) {
                            FieldStorage::Store(fields, layout.field_count(), index,
                                                initial_fields.subspan(index).front());
                          }
                        });
}

void Heap::Publish(HeapObject& object, const Allocation& allocation) noexcept {
  object.mark_bit = barrier_state_.mark_bit;
  object.allocation_size = allocation.size;
  object.size_class = allocation.size_class;
  object.next = objects_;
  if (phase() == GcPhase::Mark && !IsLeafObject(object.kind)) {
    object.gray_next = gray_;
    gray_ = &object;
  } else if (phase() == GcPhase::Sweep) {
    ProcessGrayObject(object);
    DrainGray();
  }
  objects_ = &object;
  ++object_count_;
}

void Heap::StartCollection() noexcept {
  if (IsCollecting()) {
    return;
  }

  barrier_state_.mark_bit ^= 1U;
  gray_ = nullptr;
  sweep_link_ = nullptr;
  barrier_state_.phase = GcPhase::Mark;
  MarkRoots();
}

void Heap::Step(std::size_t work_budget) noexcept {
  if (work_budget == 0) {
    return;
  }

  while (work_budget != 0 && IsCollecting()) {
    if (phase() == GcPhase::Mark) {
      if (gray_ != nullptr) {
        HeapObject* object = gray_;
        gray_ = object->gray_next;
        object->gray_next = nullptr;
        ProcessGrayObject(*object);
        --work_budget;
        continue;
      }
      // 执行可在增量步骤之间改写根槽位；进入清扫前重新捕获当前根。
      MarkRoots();
      if (gray_ != nullptr) {
        continue;
      }
      barrier_state_.phase = GcPhase::Sweep;
      sweep_link_ = &objects_;
      continue;
    }

    HeapObject* object = *sweep_link_;
    if (object == nullptr) {
      work_budget = ReleaseExcessCachedPages(work_budget);
      if (work_budget == 0) {
        return;
      }
      FinishSweep();
      continue;
    }

    if (object->mark_bit != barrier_state_.mark_bit) {
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
  Step(std::numeric_limits<std::size_t>::max());
}

void Heap::Collect() noexcept {
  StartCollection();
  FinishCollection();
}

void Heap::AppendExternalRoots(std::vector<ExternalResourceInfo>& result) const {
  for (auto* root = roots_; root != nullptr; root = root->next_) {
    const auto kind = root->resource_kind_;
    if (!kind) {
      continue;
    }
    result.push_back({.kind = *kind, .identity = root->identity_, .references = 1});
  }
}

void Heap::MarkRoots() noexcept {
  for (auto* frame = native_stack_roots_; frame != nullptr; frame = frame->previous) {
    for (const Value value : std::span(frame->values, frame->count)) {
      MarkValue(value);
    }
  }
  for (auto* root = future_roots_; root != nullptr; root = root->next_) {
    root->Trace(*this);
  }
  for (RootRange* root = roots_; root != nullptr; root = root->next_) {
    MarkRoot(*root);
  }
}

void Heap::MarkRoot(const RootRange& root) noexcept {
  if (root.kind_ == RootRange::Kind::Values) {
    for (const Value value : std::span(static_cast<const Value*>(root.data_), root.count_)) {
      MarkValue(value);
    }
  } else if (root.kind_ == RootRange::Kind::Strings) {
    for (auto* string : std::span(static_cast<StringObject* const*>(root.data_), root.count_)) {
      MarkReference(string);
    }
  } else if (root.kind_ == RootRange::Kind::InstanceFields) {
    const auto& storage = *static_cast<const InstanceStorage*>(root.data_);
    const auto fields = storage.fields();
    for (std::uint32_t index = 0; index < storage.count; ++index) {
      MarkValue(FieldStorage::Load(fields, storage.count, index));
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
  if (object.mark_bit == barrier_state_.mark_bit) {
    return;
  }
  object.mark_bit = barrier_state_.mark_bit;
  if (IsLeafObject(object.kind)) {
    return;
  }
  object.gray_next = gray_;
  gray_ = &object;
}

void Heap::ProcessGrayObject(HeapObject& object) noexcept {
  if (object.layout != nullptr) {
    MarkReference(object.layout->program_owner_);
  }
  switch (object.kind) {
    case ObjectKind::Instance:
      break;
    case ObjectKind::RegExp:
    case ObjectKind::RegExpMatch:
      MarkRegExp(object);
      return;
    case ObjectKind::ByteData:
      MarkByteData(object);
      return;
    case ObjectKind::ValueSlots:
      for (const auto value : ValueSlots(object)) {
        MarkValue(value);
      }
      return;
    case ObjectKind::NativeProgram:
      MarkNativeProgram(object);
      return;
    case ObjectKind::Reflection: {
      const auto& handle = *StorageAfter<ReflectionHandle>(object);
      MarkValue(handle.anchor);
      handle.lookup_cache.TraceValues([this](Value value) { MarkValue(value); });
      return;
    }
    case ObjectKind::Future:
    case ObjectKind::Completer:
      StorageAfter<Future>(object)->Trace(*this);
      return;
    case ObjectKind::Exception:
      MarkObject(*ExceptionData::From(object).payload);
      return;
    case ObjectKind::Collection:
      MarkCollection(object);
      return;
    case ObjectKind::Array:
      MarkArray(object);
      return;
    default:
      return;
  }
  if (auto* environment = NativeEnvironment(object)) {
    MarkObject(*environment);
  }
  const auto fields = ObjectFieldStorage(object);
  const auto count = object.layout->field_count();
  for (std::uint32_t index : object.layout->reference_fields()) {
    MarkValue(FieldStorage::Load(fields, count, index));
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

void Heap::DrainGrayIfSweeping() noexcept {
  if (phase() == GcPhase::Sweep) {
    DrainGray();
  }
}

void Heap::WriteBarrierSlow(Value value) noexcept {
  MarkValue(value);
  DrainGrayIfSweeping();
}

void Heap::WriteRootBarrier(std::span<const Value> values) noexcept {
  if (!IsCollecting()) {
    return;
  }
  for (const auto value : values) {
    MarkValue(value);
  }
  DrainGrayIfSweeping();
}

void Heap::WriteRootBarrier(const Future& future) noexcept {
  if (!IsCollecting()) {
    return;
  }
  future.Trace(*this);
  DrainGrayIfSweeping();
}

Heap::Allocation Heap::AllocateRaw(std::size_t requested_size) noexcept {
  const auto [block_size, class_index] = SelectBlockClass(requested_size);
  if (!CanAllocate(block_size)) {
    return {};
  }
  void* memory = nullptr;

  if (class_index < available_pages_.size()) {
    memory = AllocatePooledBlock(class_index);
  } else {
    memory = ::operator new(block_size, std::nothrow);
    if (memory != nullptr) {
      committed_bytes_ += block_size;
    }
  }
  if (memory == nullptr) {
    return {};
  }

  RecordAllocation(block_size);
  return {
      .memory = memory,
      .size = static_cast<std::uint32_t>(block_size),
      .size_class = static_cast<std::uint8_t>(class_index),
  };
}

void* Heap::AllocatePooledBlock(std::size_t class_index) noexcept {
  auto& available = available_pages_[class_index];
  auto* page = available;
  if (page == nullptr) {
    page = empty_pages_[class_index];
    if (page != nullptr) {
      internal::UnlinkIntrusive(*page, &BlockPage::next, &BlockPage::previous_link);
    } else {
      auto* memory = internal::AllocateHeapBlockPage();
      if (memory == nullptr) {
        return nullptr;
      }
      page = std::construct_at(static_cast<BlockPage*>(memory));
      committed_bytes_ += kBlockPageBytes;
    }
    internal::PrependIntrusive(*page, available, &BlockPage::next, &BlockPage::previous_link);
  }
  const auto block_size = kSizeClasses[class_index];
  void* memory = nullptr;
  if (auto* block = page->free_blocks) {
    page->free_blocks = block->next;
    memory = block;
    std::destroy_at(block);
  } else {
    auto storage = std::span(std::as_writable_bytes(std::span(page, 1)).data(), kBlockPageBytes);
    memory = storage.subspan(sizeof(BlockPage) + (page->initialized * block_size)).data();
    ++page->initialized;
  }
  ++page->used;
  pooled_live_bytes_ += block_size;
  if (page->used == (kBlockPageBytes - sizeof(BlockPage)) / block_size) {
    internal::UnlinkIntrusive(*page, &BlockPage::next, &BlockPage::previous_link);
  }
  return memory;
}

void Heap::ReleasePooledBlock(void* memory, std::size_t class_index) noexcept {
  pooled_live_bytes_ -= kSizeClasses[class_index];
  const auto address = std::bit_cast<std::uintptr_t>(memory) & ~(kBlockPageBytes - 1U);
  auto* page = std::bit_cast<BlockPage*>(address);
  const auto capacity = (kBlockPageBytes - sizeof(BlockPage)) / kSizeClasses[class_index];
  if (page->used == capacity) {
    internal::PrependIntrusive(*page, available_pages_[class_index], &BlockPage::next,
                               &BlockPage::previous_link);
  }
  page->free_blocks = std::construct_at(static_cast<FreeBlock*>(memory), page->free_blocks);
  --page->used;
  if (page->used == 0U) {
    internal::UnlinkIntrusive(*page, &BlockPage::next, &BlockPage::previous_link);
    internal::PrependIntrusive(*page, empty_pages_[class_index], &BlockPage::next,
                               &BlockPage::previous_link);
  }
}

void Heap::ReleaseRaw(HeapObject& object) noexcept {
  const auto block_size = object.allocation_size;
  const auto class_index = object.size_class;
  allocated_bytes_ -= block_size;

  switch (object.kind) {
    case ObjectKind::Instance:
      ReleaseInstance(object);
      break;
    case ObjectKind::Host:
      ReleaseHostObject(object);
      break;
    case ObjectKind::RegExp:
      ReleaseRegExp(object);
      break;
    case ObjectKind::NativeProgram:
      ReleaseNativeProgram(object);
      break;
    case ObjectKind::MutableString:
      ReleaseMutableString(object);
      break;
    case ObjectKind::Collection:
      ReleaseCollection(object);
      break;
    case ObjectKind::Array:
      ReleaseArray(object);
      break;
    case ObjectKind::Future:
    case ObjectKind::Completer: {
      std::destroy_at(StorageAfter<Future>(object));
      break;
    }
    case ObjectKind::Exception: {
      std::destroy_at(&ExceptionData::From(object));
      break;
    }
    case ObjectKind::Timer: {
      const std::unique_ptr<Timer> owned(static_cast<Timer*>(object.native_payload));
      break;
    }
    case ObjectKind::CancellationToken: {
      const std::unique_ptr<CancellationToken> owned(
          static_cast<CancellationToken*>(object.native_payload));
      break;
    }
    case ObjectKind::ValueSlots:
      ReleaseExternalStorage(object.native_payload, object.length * sizeof(Value));
      break;
    default:
      break;
  }
  void* memory = &object;
  // StringObject 仅含可平凡析构的对象头；复用或释放存储即结束其生命周期。
  std::destroy_at(&object);
  ReleaseBlock({.memory = memory, .size = block_size, .size_class = class_index});
}

void Heap::ReleaseBlock(const Allocation& allocation) noexcept {
  if (allocation.size_class < available_pages_.size()) {
    ReleasePooledBlock(allocation.memory, allocation.size_class);
    return;
  }

  committed_bytes_ -= allocation.size;
  ::operator delete(allocation.memory);
}

std::pair<std::size_t, std::size_t> Heap::SelectBlockClass(std::size_t requested_size) noexcept {
  if (requested_size > kSizeClasses.back()) {
    return {requested_size, kSizeClasses.size()};
  }
  const auto block_size = std::bit_ceil(std::max(requested_size, kSizeClasses.front()));
  auto class_index =
      static_cast<std::size_t>(std::bit_width(block_size) - std::bit_width(kSizeClasses.front())) *
      2U;
  if (class_index != 0U && requested_size <= kSizeClasses[class_index - 1U]) {
    --class_index;
  }
  return {kSizeClasses[class_index], class_index};
}

bool Heap::ShouldCollect(std::size_t next_size) const noexcept {
  return next_size > gc_threshold_ || allocated_bytes_ > gc_threshold_ - next_size;
}

bool Heap::CanAllocate(std::size_t bytes) const noexcept {
  return allocation_statistics_.count < allocation_count_limit_ && bytes <= allocation_limit_ &&
         allocated_bytes_ <= allocation_limit_ - bytes;
}

void Heap::MaybeCollect(std::size_t next_size) noexcept {
  if (!IsCollecting() && ShouldCollect(next_size)) {
    StartCollection();
  }
  if (IsCollecting() && gc_scheduling_ == GcScheduling::kAutomatic) {
    Step(kAutomaticWork);
  }
}

void Heap::ReleasePage(BlockPage& page) noexcept {
  internal::UnlinkIntrusive(page, &BlockPage::next, &BlockPage::previous_link);
  std::destroy_at(&page);
  internal::ReleaseHeapBlockPage(&page);
  committed_bytes_ -= kBlockPageBytes;
}

std::size_t Heap::ReleaseExcessCachedPages(std::size_t work_budget) noexcept {
  // 复用预算只跟随池中活槽；原生资源压力和独立大块不会复用小对象页，不能放大缓存。
  // 释放也消耗增量工作预算，页面卸载不会把历史高峰变成一次同步清理停顿。
  const auto cache_budget = std::max(minimum_gc_threshold_, pooled_live_bytes_);
  for (std::size_t index = empty_pages_.size(); index != 0 && work_budget != 0; --index) {
    auto& head = empty_pages_[index - 1U];
    while (head != nullptr && committed_bytes_ - allocated_bytes_ > cache_budget &&
           work_budget != 0) {
      ReleasePage(*head);
      --work_budget;
    }
  }
  return work_budget;
}

void Heap::FinishSweep() noexcept {
  ReleaseRetiredNativePrograms();
  if (isolate_ != nullptr) {
    isolate_->CollectNativeMetadata();
  }
  barrier_state_.phase = GcPhase::Idle;
  sweep_link_ = nullptr;
  const auto threshold = [&] {
    const auto doubled_live_size = allocated_bytes_ > std::numeric_limits<std::size_t>::max() / 2
                                       ? std::numeric_limits<std::size_t>::max()
                                       : allocated_bytes_ * 2;
    return std::max(minimum_gc_threshold_, doubled_live_size);
  };
  // 先允许缩表分配，再按缩表后的实际存量确定下一轮回收阈值。
  gc_threshold_ = threshold();
  host_index_.CompactAfterCollection();
  gc_threshold_ = threshold();
}

void Heap::DestroyAll() noexcept {
  while (objects_ != nullptr) {
    HeapObject* object = objects_;
    objects_ = object->next;
    ReleaseRaw(*object);
  }
  object_count_ = 0;
  ReleaseRetiredNativePrograms();

  for (auto& page : empty_pages_) {
    while (page != nullptr) {
      ReleasePage(*page);
    }
  }
  assert(allocated_bytes_ == 0);
  assert(committed_bytes_ == 0);
}

}  // namespace vela::runtime
