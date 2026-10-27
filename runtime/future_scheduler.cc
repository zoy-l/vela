#include "runtime/future.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <tuple>
#include <utility>

#include "runtime/value.h"

namespace vela::runtime {

void Timer::Cancel() noexcept {
  if (state_->cancelled.exchange(true)) {
    return;
  }
  auto* scheduler = state_->scheduler;
  if (scheduler != nullptr && !scheduler->IsOwnerThread()) {
    scheduler->PostMicrotask([state = state_] { state->callback = {}; });
  } else {
    state_->callback = {};
  }
}

bool Timer::IsCancelled() const noexcept {
  return state_->cancelled.load();
}

FutureScheduler::~FutureScheduler() {
  Shutdown();
}

void FutureScheduler::Shutdown() {
  assert(IsOwnerThread());
  ReceiveMessages();
  host_events_.clear();
  microtasks_.clear();
  checkpoints_.clear();
  while (!events_.empty()) {
    events_.pop();
  }
}

void FutureScheduler::AddSuspendedFrame(std::size_t saved_bytes) noexcept {
  ++suspended_frames_;
  saved_state_bytes_ += saved_bytes;
}

void FutureScheduler::RemoveSuspendedFrame(std::size_t saved_bytes) noexcept {
  --suspended_frames_;
  saved_state_bytes_ -= saved_bytes;
}

void FutureScheduler::PostMicrotask(std::function<void()> task) {
  if (IsOwnerThread()) {
    microtasks_.push_back(std::move(task));
    return;
  }
  auto message = std::make_unique<Message>();
  message->action = std::move(task);
  auto* head = incoming_.load(std::memory_order_relaxed);
  while (true) {
    message->next = head;
    if (incoming_.compare_exchange_weak(head, message.get(), std::memory_order_release,
                                        std::memory_order_relaxed)) {
      break;
    }
  }
  std::ignore = message.release();
  wakeup_.release();
}

void FutureScheduler::ReceiveMessages() {
  auto* message = incoming_.exchange(nullptr, std::memory_order_acquire);
  Message* ordered = nullptr;
  while (message != nullptr) {
    auto* next = message->next;
    message->next = ordered;
    ordered = message;
    message = next;
  }
  while (ordered != nullptr) {
    std::unique_ptr<Message> current(ordered);
    ordered = ordered->next;
    host_events_.push_back(std::move(current->action));
  }
}

void FutureScheduler::SetUnhandledErrorHandler(ErrorCallback callback) {
  assert(IsOwnerThread());
  unhandled_error_handler_ = std::move(callback);
}

void FutureScheduler::SetHandledErrorHandler(ErrorCallback callback) {
  assert(IsOwnerThread());
  handled_error_handler_ = std::move(callback);
}

void FutureScheduler::NotifyUnhandledError(const Future& future, ::vela::runtime::Value error) {
  if (unhandled_error_handler_) {
    unhandled_error_handler_(future, error);
  }
}

void FutureScheduler::NotifyHandledError(const Future& future, ::vela::runtime::Value error) {
  if (handled_error_handler_) {
    handled_error_handler_(future, error);
  }
}

Completer FutureScheduler::CreateCompleter() {
  return Completer(Future(this));
}

HostEventSource::HostEventSource(FutureScheduler& scheduler) : scheduler_(&scheduler) {
  ++scheduler_->host_sources_;
}

HostEventSource::~HostEventSource() {
  Release();
}

void HostEventSource::Release() noexcept {
  if (live_.exchange(false)) {
    --scheduler_->host_sources_;
    scheduler_->wakeup_.release();
  }
}

Completer FutureScheduler::CreateHostCompleter() {
  return {Future(this), std::make_shared<HostEventSource>(*this)};
}

Timer FutureScheduler::After(std::chrono::milliseconds delay, std::function<void()> callback) {
  assert(IsOwnerThread());
  auto state = std::make_shared<Timer::State>();
  state->callback = std::move(callback);
  state->scheduler = this;
  events_.push(Event{
      .deadline = Clock::now() + delay,
      .sequence = next_sequence_++,
      .interval = std::chrono::milliseconds(0),
      .state = state,
  });
  return Timer(std::move(state));
}

Timer FutureScheduler::Periodic(std::chrono::milliseconds interval,
                                std::function<void()> callback) {
  assert(IsOwnerThread());
  auto state = std::make_shared<Timer::State>();
  state->callback = std::move(callback);
  state->scheduler = this;
  events_.push(Event{
      .deadline = Clock::now() + interval,
      .sequence = next_sequence_++,
      .interval = interval,
      .state = state,
  });
  return Timer(std::move(state));
}

void FutureScheduler::DrainMicrotasks() {
  while (!microtasks_.empty() || !checkpoints_.empty()) {
    auto& queue = microtasks_.empty() ? checkpoints_ : microtasks_;
    auto task = std::move(queue.front());
    queue.pop_front();
    task();
  }
}

bool FutureScheduler::PumpOne(FuturePumpMode mode) {
  assert(IsOwnerThread());
  while (true) {
    if (!microtasks_.empty() || !checkpoints_.empty()) {
      DrainMicrotasks();
      return true;
    }
    ReceiveMessages();
    if (!host_events_.empty()) {
      auto event = std::move(host_events_.front());
      host_events_.pop_front();
      event();
      DrainMicrotasks();
      return true;
    }
    while (!events_.empty() && events_.top().state->cancelled.load()) {
      events_.pop();
    }
    if (events_.empty()) {
      if (mode == FuturePumpMode::AvailableWork || host_sources_.load() == 0) {
        return false;
      }
      wakeup_.acquire();
      continue;
    }
    const auto deadline = events_.top().deadline;
    if (deadline > Clock::now()) {
      std::ignore = wakeup_.try_acquire_until(deadline);
      continue;
    }
    auto event = events_.top();
    events_.pop();
    Fire(std::move(event));
    return true;
  }
}

void FutureScheduler::Fire(Event event) {
  auto& state = *event.state;
  auto callback = event.interval.count() == 0 ? std::move(state.callback) : state.callback;
  if (callback && !state.cancelled.load()) {
    callback();
    DrainMicrotasks();
  }
  if (event.interval.count() > 0 && !state.cancelled.load()) {
    event.deadline = Clock::now() + event.interval;
    event.sequence = next_sequence_++;
    events_.push(std::move(event));
  }
}

std::size_t FutureScheduler::pending_microtasks() const noexcept {
  assert(IsOwnerThread());
  return microtasks_.size();
}

bool FutureScheduler::IsOwnerThread() const noexcept {
  return owner_ == std::this_thread::get_id();
}

bool FutureScheduler::CanSchedule(std::chrono::milliseconds duration) noexcept {
  return duration.count() >= 0 && duration <= std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  Clock::time_point::max() - Clock::now());
}

bool FutureScheduler::PumpUntil(Future& root, FuturePumpMode mode) {
  while (root.is_pending()) {
    if (!PumpOne(mode)) {
      return false;
    }
  }
  return true;
}

}  // namespace vela::runtime
