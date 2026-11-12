#include "runtime/native/native_control.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <semaphore>
#include <utility>

#include <absl/functional/any_invocable.h>

#include "base/execution_status.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_debugger.h"

namespace vela::runtime {
namespace {

constexpr std::uint8_t kStopRequested = 1U << 0U;
constexpr std::uint8_t kQueuedAction = 1U << 1U;
constexpr std::uint8_t kPendingPublication = 1U << 2U;
constexpr std::uint8_t kDebuggerAttached = 1U << 3U;

}  // namespace

NativeSafepointEndpoint::NativeSafepointEndpoint(std::shared_ptr<NativeSafepointQueue> queue)
    : queue_(std::move(queue)) {}

void NativeSafepointEndpoint::RequestStop() const noexcept {
  queue_->requests.fetch_or(kStopRequested, std::memory_order_release);
  const std::scoped_lock lock(queue_->mutex);
  queue_->closed = true;
  if (const auto wakeup = queue_->wakeup.lock()) {
    wakeup->release();
  }
}

std::expected<void, NativeSafepointError> NativeSafepointEndpoint::Post(
    absl::AnyInvocable<void()> action) const {
  const std::scoped_lock lock(queue_->mutex);
  if (queue_->closed) {
    return std::unexpected(NativeSafepointError::Stopped);
  }
  queue_->actions.push_back(std::move(action));
  queue_->requests.fetch_or(kQueuedAction, std::memory_order_release);
  if (const auto wakeup = queue_->wakeup.lock()) {
    wakeup->release();
  }
  return {};
}

NativeControl::NativeControl(std::weak_ptr<std::counting_semaphore<>> wakeup)
    : queue_(std::make_shared<NativeSafepointQueue>()), state_{.requests = &queue_->requests} {
  queue_->wakeup = std::move(wakeup);
}

NativeControl::~NativeControl() {
  // 关闭后的端点可独立存活；携带所有权的动作仍在所属线程释放。
  std::deque<absl::AnyInvocable<void()>> retired;
  {
    const std::scoped_lock lock(queue_->mutex);
    queue_->closed = true;
    retired.swap(queue_->actions);
    queue_->requests.fetch_and(static_cast<std::uint8_t>(~kQueuedAction),
                               std::memory_order_relaxed);
  }
}

NativeSafepointEndpoint NativeControl::endpoint() const {
  return NativeSafepointEndpoint(queue_);
}

void NativeControl::SetDebugger(NativeDebugger* debugger) noexcept {
  debugger_ = debugger;
  if (debugger != nullptr) {
    queue_->requests.fetch_or(kDebuggerAttached, std::memory_order_release);
  } else {
    queue_->requests.fetch_and(static_cast<std::uint8_t>(~kDebuggerAttached),
                               std::memory_order_release);
  }
}

void NativeControl::RequestStop() const noexcept {
  endpoint().RequestStop();
}

bool NativeControl::HasHostBorrow() const noexcept {
  return state_.host_borrows != 0;
}

void NativeControl::EnterInitialization() noexcept {
  ++state_.initializations;
}

void NativeControl::ExitInitialization() noexcept {
  --state_.initializations;
}

bool NativeControl::CanPublish() const noexcept {
  return !IsStopRequested() && !HasHostBorrow() && !IsInitializing();
}

bool NativeControl::IsInitializing() const noexcept {
  return state_.initializations != 0;
}

std::expected<void, NativeSafepointError> NativeControl::ScheduleSafepoint(
    absl::AnyInvocable<void()> action) {
  if (IsStopRequested()) {
    return std::unexpected(NativeSafepointError::Stopped);
  }
  if (pending_) {
    return std::unexpected(NativeSafepointError::AlreadyPending);
  }
  pending_ = std::move(action);
  queue_->requests.fetch_or(kPendingPublication, std::memory_order_release);
  return {};
}

void NativeControl::CancelSafepoint() noexcept {
  pending_ = {};
  queue_->requests.fetch_and(static_cast<std::uint8_t>(~kPendingPublication),
                             std::memory_order_release);
}

ExecutionStatus NativeControl::ProcessSafepoint(NativeExecution* execution) {
  if (IsStopRequested() || HasHostBorrow() || processing_) {
    return Poll();
  }
  processing_ = true;
  absl::AnyInvocable<void()> queued;
  if ((queue_->requests.load(std::memory_order_acquire) & kQueuedAction) != 0) {
    const std::scoped_lock lock(queue_->mutex);
    queued = std::move(queue_->actions.front());
    queue_->actions.pop_front();
    if (queue_->actions.empty()) {
      queue_->requests.fetch_and(static_cast<std::uint8_t>(~kQueuedAction),
                                 std::memory_order_release);
    }
  }
  if (queued) {
    queued();
  } else if (CanPublish() && pending_) {
    auto action = std::move(pending_);
    queue_->requests.fetch_and(static_cast<std::uint8_t>(~kPendingPublication),
                               std::memory_order_release);
    action();
  }
  processing_ = false;
  auto* debugger = debugger_;
  if (debugger != nullptr && execution != nullptr && execution->current_frame() != nullptr &&
      !debugging_ && !IsStopRequested()) {
    debugging_ = true;
    const NativeDebugContext context(*execution);
    debugger->AtSafepoint(context);
    debugging_ = false;
  }
  return Poll();
}

bool NativeControl::IsStopRequested() const noexcept {
  return (queue_->requests.load(std::memory_order_acquire) & kStopRequested) != 0;
}

NativeInitializationScope::NativeInitializationScope(NativeControl& control) noexcept
    : control_(&control) {
  control_->EnterInitialization();
}

NativeInitializationScope::~NativeInitializationScope() {
  control_->ExitInitialization();
}

ExecutionStatus NativeControl::Poll() const noexcept {
  return IsStopRequested() && !HasHostBorrow() ? ExecutionStatus::ExecutionStopped
                                               : ExecutionStatus::Returned;
}

extern "C" std::uint8_t vela_native_v3_safepoint(NativeExecution* execution) noexcept {
  return static_cast<std::uint8_t>(
      execution->isolate().native_control().ProcessSafepoint(execution));
}

}  // namespace vela::runtime
