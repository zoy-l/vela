#include "runtime/native_abi.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "runtime/exception.h"
#include "runtime/external_resources.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_deoptimization.h"
#include "runtime/native_dispatch.h"
#include "runtime/native_instance.h"
#include "runtime/value.h"

namespace vela::runtime {

NativeFrame::NativeFrame(Isolate& isolate, NativeFrameSize size) noexcept
    : allocated_roots_(size.root_count),
      storage_bytes_(size.storage_bytes),
      storage_(storage_bytes_ == 0 ? nullptr : ::operator new(storage_bytes_, std::nothrow)) {
  if (IsAllocated() && size.root_count != 0) {
    const auto roots = allocated_roots_.values();
    root_range_.Attach(isolate.heap(), roots.data(), roots.size());
  }
}

NativeFrame::~NativeFrame() {
  ::operator delete(storage_);
}

NativeExecution::NativeExecution(NativeInstance& instance) noexcept
    : instance_(&instance),
      context_(instance.isolate()),
      exception_root_(isolate().heap(), &pending_exception_, 1, RootOwner::Runtime),
      execution_resource_(isolate().external_resources(), ExternalResourceKind::Execution) {
  if (instance.image().native_mode == static_cast<std::uint32_t>(NativeProgramMode::Reloadable)) {
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

ExecutionStatus NativeExecution::PrepareEntry() noexcept {
  if (frame_count_ == 0) {
    pending_exception_ = Value::Null();
    exception_allocation_failed_ = false;
  }
  if (frame_count_ == kMaxCallFrames) {
    return ExecutionStatus::StackOverflow;
  }
  return ExecutionStatus::Returned;
}

ExecutionStatus NativeExecution::Enter(NativeTraceFrame& frame, std::uint32_t function) noexcept {
  const auto prepared = PrepareEntry();
  if (prepared != ExecutionStatus::Returned) {
    return prepared;
  }
  MaterializeFrame(frame, function);
  return ExecutionStatus::Returned;
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
  auto* physical = current_frame_;
  Exit();
  const auto status = checkpoint.recovery(this, &checkpoint, result);
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
    return static_cast<std::uint8_t>(ExecutionStatus::OutOfMemory);
  }
  *access = {.roots = frame->roots().data(), .storage = frame->storage()};
  return static_cast<std::uint8_t>(ExecutionStatus::Returned);
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
  std::construct_at(range, execution->isolate().heap(), roots, size, RootOwner::Runtime);
}

extern "C" void vela_native_v1_roots_exit(RootRange* range) noexcept {
  std::destroy_at(range);
}

ExecutionStatus NativeExecution::HandleLeafFailure(TracePosition position,
                                                   ExecutionStatus status,
                                                   std::span<const Value> roots) {
  const RootRange retained(isolate().heap(), roots.data(), roots.size(), RootOwner::Runtime);
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
  return static_cast<std::uint8_t>(execution->HandleLeafFailure(
      DecodeNativeTracePosition(position), static_cast<ExecutionStatus>(status),
      std::span(roots, static_cast<std::size_t>(root_count))));
}

}  // namespace vela::runtime
