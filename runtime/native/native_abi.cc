#include "runtime/native/native_abi.h"
#include <optional>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_deoptimization.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_materialization.h"
#include "runtime/native/native_stack.h"
#include "runtime/value/value.h"

namespace vela::runtime {

NativeFrame::NativeFrame(Isolate& isolate, NativeFrameSize size) noexcept {
  constexpr auto kMaximum = std::numeric_limits<std::size_t>::max();
  if (size.root_count > (kMaximum - size.storage_bytes) / sizeof(Value)) {
    allocation_bytes_ = kMaximum;
    return;
  }
  const auto root_bytes = size.root_count * sizeof(Value);
  allocation_bytes_ = root_bytes + size.storage_bytes;
  if (allocation_bytes_ == 0) {
    return;
  }
  allocation_ =
      ::operator new(allocation_bytes_, std::align_val_t{kNativeFrameAlignment}, std::nothrow);
  if (allocation_ == nullptr) {
    return;
  }
  static_assert(sizeof(Value) % kNativeFrameAlignment == 0);
  roots_ = {static_cast<Value*>(allocation_), size.root_count};
  std::uninitialized_value_construct_n(roots_.data(), roots_.size());
  storage_ = size.storage_bytes == 0 ? nullptr : std::as_writable_bytes(roots_).last(0).data();
  if (!roots_.empty()) {
    root_range_.Attach(isolate.heap(), roots_.data(), roots_.size());
  }
}

NativeFrame::~NativeFrame() {
  root_range_.Detach();
  ::operator delete(allocation_, std::align_val_t{kNativeFrameAlignment});
}

NativeExecution::NativeExecution(NativeInstance& instance) noexcept
    : NativeExecution(instance, nullptr) {}

NativeExecution::NativeExecution(NativeInstance& instance, const NativeExecution* parent) noexcept
    : instance_(&instance),
      context_(instance.isolate(), *this),
      control_state_(&isolate().native_control().state()),
      barrier_state_(&isolate().heap().barrier_state()),
      stack_roots_(isolate().heap().native_stack_roots_head()),
      execution_resource_(isolate().external_resources(), ExternalResourceKind::Execution) {
  const auto bounds = CurrentNativeStackBounds();
  if (parent != nullptr) {
    current_frame_ = parent->current_frame_;
    frame_count_ = parent->frame_count_;
    base_frame_count_ = frame_count_;
  }
  if (!bounds) {
    stack_status_ = bounds.error();
  } else if (bounds->upper - bounds->lower <= kNativeStackReserveBytes) {
    stack_status_ = ExecutionStatus::StackOverflow;
  } else {
    stack_limit_ = bounds->lower + kNativeStackReserveBytes;
    stack_status_ = ExecutionStatus::Returned;
  }
  if (instance.image().IsReloadable()) {
    isolate().native_dispatch().RegisterExecution(*this);
  }
}

NativeExecution::~NativeExecution() {
  if (version_previous_link_ != nullptr) {
    NativeDispatch::UnregisterExecution(*this);
  }
}

NativeInstanceScope::NativeInstanceScope(NativeExecution& execution,
                                         NativeInstance& instance) noexcept
    : execution_(&execution), previous_(execution.instance_) {
  execution.instance_ = &instance;
}

NativeInstanceScope::~NativeInstanceScope() {
  execution_->instance_ = previous_;
}

void NativeExecution::MaterializeFrame(NativeTraceFrame& frame, std::uint32_t function) noexcept {
  frame = {
      .position = {.function = function},
      .previous = current_frame_,
      .image = &instance().image(),
  };
  current_frame_ = &frame;
  ++frame_count_;
}

void NativeExecution::Exit() noexcept {
  --frame_count_;
  current_frame_ = current_frame_->previous;
}

std::uint8_t NativeExecution::Recover(NativeCheckpoint& checkpoint, void* result) noexcept {
  if (NativeMaterialization::HasVirtualObjects(checkpoint)) {
    const NativeInitializationScope initialization(isolate().native_control());
    auto prepared = NativeMaterialization::Prepare(isolate().heap(), checkpoint);
    if (!prepared) {
      // 保留原异常的根；OOM 中止恢复，不能让外层 catch 把旧异常当作新恢复结果。
      exception_allocation_failed_ = true;
      return ToNativeStatus(ExecutionStatus::OutOfMemory);
    }
    (*prepared)->Commit();
  }
  // 从此只运行基线恢复；进入其第一个安全点前不会分配，那里将登记规范活跃值。
  // 清除优化专用的字段根，避免旧物理帧在恢复执行期间额外保活死亡子图。
  const auto& layout = *checkpoint.layout;
  std::ranges::fill(std::span(checkpoint.roots, static_cast<std::size_t>(layout.root_count)),
                    Value::Null());
  auto* physical = current_frame_;
  Exit();
  const auto status = layout.recovery(this, &checkpoint, result);
  current_frame_ = physical;
  ++frame_count_;
  return status;
}

extern "C" std::uint8_t vela_native_v1_frame_storage(NativeExecution* execution,
                                                     std::uint64_t root_count,
                                                     std::uint64_t storage_bytes,
                                                     NativeFrame* frame,
                                                     NativeFrameAccess* access) noexcept {
  std::construct_at(frame, execution->isolate(),
                    NativeFrameSize{
                        .root_count = static_cast<std::size_t>(root_count),
                        .storage_bytes = static_cast<std::size_t>(storage_bytes),
                    });
  if (!frame->IsAllocated()) {
    std::destroy_at(frame);
    return ToNativeStatus(ExecutionStatus::OutOfMemory);
  }
  *access = {.roots = frame->roots().data(), .storage = frame->storage()};
  return ToNativeStatus(ExecutionStatus::Returned);
}

extern "C" void vela_native_v1_frame_release(NativeFrame* frame) noexcept {
  std::destroy_at(frame);
}

extern "C" void vela_native_v1_roots_enter(NativeExecution* execution,
                                           Value* roots,
                                           std::uint64_t count,
                                           RootRange* range) noexcept {
  const auto size = static_cast<std::size_t>(count);
  std::uninitialized_value_construct_n(roots, size);
  std::construct_at(range, std::nullopt);
  range->Attach(execution->isolate().heap(), roots, size);
}

extern "C" void vela_native_v1_roots_exit(RootRange* range) noexcept {
  std::destroy_at(range);
}

ExecutionStatus NativeExecution::HandleLeafFailure(TracePosition position,
                                                   ExecutionStatus status,
                                                   std::span<const Value> roots) {
  const RootRange retained(isolate().heap(), roots.data(), roots.size(), std::nullopt);
  NativeTraceFrame frame;
  MaterializeFrame(frame, position.function);
  frame.position.instruction = position.instruction;
  const auto failure = HandleFailure(status);
  Exit();
  return failure;
}

extern "C" std::uint8_t vela_native_v2_leaf_failure(NativeExecution* execution,
                                                    std::uint64_t position,
                                                    std::uint8_t status,
                                                    const Value* roots,
                                                    std::uint64_t root_count) {
  return ToNativeStatus(execution->HandleLeafFailure(
      DecodeNativeTracePosition(position), static_cast<ExecutionStatus>(status),
      std::span(roots, static_cast<std::size_t>(root_count))));
}

}  // namespace vela::runtime
