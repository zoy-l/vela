#include "runtime/native_abi.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "runtime/isolate.h"
#include "runtime/value.h"

namespace vela::runtime {

NativeFrame::NativeFrame(Isolate& isolate, std::size_t root_count) noexcept {
  if (root_count != 0) {
    roots_ = static_cast<Value*>(::operator new(root_count * sizeof(Value), std::nothrow));
    if (roots_ == nullptr) {
      return;
    }
    std::uninitialized_value_construct_n(roots_, root_count);
    root_count_ = root_count;
  }
  root_range_.Attach(isolate.heap(), roots_, root_count_);
}

NativeFrame::~NativeFrame() {
  root_range_.Detach();
  std::destroy_n(roots_, root_count_);
  ::operator delete(roots_);
}

NativeExecution::~NativeExecution() {
  while (frame_count_ != 0) {
    Exit();
  }
}

ExecutionStatus NativeExecution::Enter(std::size_t root_count) noexcept {
  if (frame_count_ == kMaxCallFrames) {
    return ExecutionStatus::StackOverflow;
  }
  if (root_count > std::numeric_limits<std::size_t>::max() / sizeof(Value)) {
    return ExecutionStatus::OutOfMemory;
  }
  std::unique_ptr<NativeFrame> frame(new (std::nothrow) NativeFrame(*isolate_, root_count));
  if (frame == nullptr || frame->roots().size() != root_count) {
    return ExecutionStatus::OutOfMemory;
  }
  frames_.at(frame_count_++) = std::move(frame);
  return ExecutionStatus::Returned;
}

void NativeExecution::Exit() noexcept {
  frames_.at(--frame_count_).reset();
}

std::span<Value> NativeExecution::current_roots() noexcept {
  return frame_count_ == 0 ? std::span<Value>() : frames_.at(frame_count_ - 1)->roots();
}

extern "C" std::uint8_t vela_native_v1_frame_enter(NativeExecution* execution,
                                                   std::uint64_t root_count) noexcept {
  if (execution == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::InvalidIsolate);
  }
  return static_cast<std::uint8_t>(execution->Enter(static_cast<std::size_t>(root_count)));
}

extern "C" void vela_native_v1_frame_exit(NativeExecution* execution) noexcept {
  execution->Exit();
}

}  // namespace vela::runtime
