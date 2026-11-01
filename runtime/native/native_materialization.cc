#include "runtime/native/native_materialization.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/instance_storage.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_deoptimization.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_program_state.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

NativeMaterialization::NativeMaterialization(Heap& heap, NativeCheckpoint& checkpoint) noexcept
    : heap_(&heap), checkpoint_(&checkpoint) {}

NativeMaterialization::~NativeMaterialization() {
  roots_.Detach();
  if (!objects_.empty()) {
    std::destroy_n(objects_.data(), objects_.size());
    heap_->FreeNative(objects_.data());
  }
  if (!work_.empty()) {
    std::destroy_n(work_.data(), work_.size());
    heap_->FreeNative(work_.data());
  }
}

bool NativeMaterialization::HasVirtualObjects(const NativeCheckpoint& checkpoint) noexcept {
  const auto* materialization = checkpoint.materialization;
  if (materialization == nullptr || materialization->graph == nullptr) {
    return false;
  }
  const auto* graph = materialization->graph;
  const auto objects = graph->objects.values();
  const std::span references(materialization->references, checkpoint.layout->reference_count);
  return std::ranges::any_of(graph->aliases.values(), [&](const auto& alias) {
    const auto& object = objects.subspan(alias.object).front();
    return *references.subspan(object.storage).front() == nullptr;
  });
}

std::expected<std::unique_ptr<NativeMaterialization>, ExecutionStatus>
NativeMaterialization::Prepare(Heap& heap, NativeCheckpoint& checkpoint) noexcept {
  std::unique_ptr<NativeMaterialization> prepared(new (std::nothrow)
                                                      NativeMaterialization(heap, checkpoint));
  if (prepared == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  if (const auto status = prepared->PrepareStorage(); status != ExecutionStatus::Returned) {
    return std::unexpected(status);
  }
  if (const auto status = prepared->BuildGraph(); status != ExecutionStatus::Returned) {
    return std::unexpected(status);
  }
  return prepared;
}

ExecutionStatus NativeMaterialization::PrepareStorage() noexcept {
  const auto& materialization = *checkpoint_->materialization;
  const auto count = materialization.graph->objects.values().size();
  // 最后一个槽保活解释旧字段身份所需的程序状态及其代码批次。
  auto* values = static_cast<Value*>(heap_->AllocateNative((count + 1) * sizeof(Value)));
  if (values == nullptr) {
    return ExecutionStatus::OutOfMemory;
  }
  objects_ = {values, count + 1};
  std::uninitialized_value_construct_n(values, count + 1);
  objects_.back() = Value::Object(materialization.program);
  roots_.Attach(*heap_, objects_.data(), objects_.size());
  auto* work = static_cast<Work*>(heap_->AllocateNative(count * sizeof(Work)));
  if (work == nullptr) {
    return ExecutionStatus::OutOfMemory;
  }
  work_ = {work, count};
  std::uninitialized_value_construct_n(work, count);
  return ExecutionStatus::Returned;
}

void NativeMaterialization::Enqueue(std::uint32_t node) noexcept {
  if (work_.subspan(node).front().queued) {
    return;
  }
  work_.subspan(node).front().queued = true;
  work_.subspan(queued_++).front().node = node;
}

ExecutionStatus NativeMaterialization::AllocateGraph() noexcept {
  const auto& materialization = *checkpoint_->materialization;
  const auto descriptions = materialization.graph->objects.values();
  const auto& layouts = Heap::NativeProgram(*materialization.program).layouts;
  for (std::size_t position = 0; position < queued_; ++position) {
    const auto node = work_.subspan(position).front().node;
    auto& value = objects_.subspan(node).front();
    if (!value.IsNull()) {
      continue;
    }
    const auto& description = descriptions.subspan(node).front();
    auto* object = heap_->AllocateInstance(*layouts.classes.at(description.class_index));
    if (object == nullptr) {
      return ExecutionStatus::OutOfMemory;
    }
    value = Value::Object(object);
    ++object_count_;
  }
  return ExecutionStatus::Returned;
}

ExecutionStatus NativeMaterialization::BuildGraph() noexcept {
  const auto& checkpoint = *checkpoint_;
  const auto& checkpoint_layout = *checkpoint.layout;
  const auto& materialization = *checkpoint.materialization;
  const auto& graph = *materialization.graph;
  const auto descriptions = graph.objects.values();
  const std::span references(materialization.references, checkpoint_layout.reference_count);
  const std::span values(checkpoint.values, checkpoint_layout.value_count);
  for (const auto& alias : graph.aliases.values()) {
    Enqueue(alias.object);
  }
  for (std::size_t position = 0; position < queued_; ++position) {
    const auto node = work_.subspan(position).front().node;
    const auto& description = descriptions.subspan(node).front();
    auto* existing = *references.subspan(description.storage).front();
    if (existing != nullptr) {
      objects_.subspan(node).front() = Value::Object(existing);
      continue;
    }
    for (const auto& field : description.fields.values()) {
      if (field.object != kNoNativeIndex) {
        Enqueue(field.object);
      }
    }
  }
  if (const auto status = AllocateGraph(); status != ExecutionStatus::Returned) {
    return status;
  }
  for (std::size_t position = 0; position < queued_; ++position) {
    const auto node = work_.subspan(position).front().node;
    const auto& description = descriptions.subspan(node).front();
    if (*references.subspan(description.storage).front() != nullptr) {
      continue;
    }
    auto& object = *objects_.subspan(node).front().AsObject();
    const auto& layout = *Heap::ClassOf(object);
    for (const auto& field : description.fields.values()) {
      const auto slot = *layout.FieldIndex(field.declaration);
      auto value = Value::Null();
      if (field.state != FieldState::Uninitialized) {
        value = field.object == kNoNativeIndex
                    ? DecodeNativeValue(values.subspan(field.value).front())
                    : objects_.subspan(field.object).front();
      }
      static_cast<void>(heap_->SetField(object, slot, value));
      Heap::InstanceFields(object).SetFieldState(slot, field.state);
    }
  }
  return ExecutionStatus::Returned;
}

void NativeMaterialization::Commit() noexcept {
  const auto& checkpoint = *checkpoint_;
  const auto& layout = *checkpoint.layout;
  const auto& materialization = *checkpoint.materialization;
  const auto& graph = *materialization.graph;
  const auto descriptions = graph.objects.values();
  const std::span references(materialization.references, layout.reference_count);
  const std::span values(checkpoint.values, layout.value_count);
  const std::span roots(checkpoint.roots, layout.root_count);
  for (std::size_t position = 0; position < queued_; ++position) {
    const auto node = work_.subspan(position).front().node;
    const auto& description = descriptions.subspan(node).front();
    const auto value = objects_.subspan(node).front();
    *references.subspan(description.storage).front() = value.AsObject();
    roots.subspan(description.root).front() = value;
  }
  for (const auto& alias : graph.aliases.values()) {
    values.subspan(alias.reg).front() = EncodeNativeValue(objects_.subspan(alias.object).front());
  }
}

std::expected<std::vector<std::unique_ptr<NativeMaterialization>>, ExecutionStatus>
NativeDispatch::PrepareMaterializations() {
  std::vector<std::unique_ptr<NativeMaterialization>> prepared;
  const NativeInitializationScope initialization(isolate_->native_control());
  for (auto* execution = executions_; execution != nullptr; execution = execution->version_next_) {
    for (auto* frame = execution->current_frame_; frame != nullptr; frame = frame->previous) {
      auto* checkpoint = frame->checkpoint;
      if (checkpoint == nullptr || !NativeMaterialization::HasVirtualObjects(*checkpoint)) {
        continue;
      }
      auto materialization = NativeMaterialization::Prepare(isolate_->heap(), *checkpoint);
      if (!materialization) {
        return std::unexpected(materialization.error());
      }
      prepared.push_back(std::move(*materialization));
    }
  }
  return prepared;
}

}  // namespace vela::runtime
