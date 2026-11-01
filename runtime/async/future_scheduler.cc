#include "runtime/async/future.h"
#include "runtime/isolate/external_resources.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/functional/any_invocable.h>

#include "runtime/async/execution_context.h"
#include "runtime/value/value.h"

namespace vela::runtime {

void Timer::Cancel() noexcept {
  if (state_ == nullptr) {
    return;
  }
  {
    const std::scoped_lock lock(state_->mutex);
    if (state_->cancelled.exchange(true)) {
      return;
    }
    auto* scheduler = state_->scheduler;
    if (scheduler != nullptr && !scheduler->IsOwnerThread()) {
      scheduler->PostMicrotask([state = state_] { Close(*state); });
      return;
    }
  }
  Close(*state_);
}

void Timer::Close(State& state) noexcept {
  std::function<void()> retired;
  FutureScheduler* scheduler = nullptr;
  {
    const std::scoped_lock lock(state.mutex);
    state.cancelled = true;
    scheduler = std::exchange(state.scheduler, nullptr);
    retired.swap(state.callback);
  }
  if (scheduler != nullptr && state.event_index != State::kNotQueued) {
    static_cast<void>(scheduler->RemoveEvent(state.event_index));
  }
}

FutureScheduler::~FutureScheduler() {
  Shutdown();
}

void FutureScheduler::Shutdown() {
  assert(IsOwnerThread());
  for (const auto& [identity, state] : completion_endpoints_) {
    FutureCompletionEndpoint::Close(*state);
  }
  completion_endpoints_.clear();
  ReceiveMessages();
  host_events_.clear();
  microtasks_.clear();
  checkpoints_.clear();
  while (!events_.empty()) {
    const auto event = RemoveEvent(0);
    Timer::Close(*event.state);
  }
}

void FutureScheduler::RetireCompletionEndpoint(std::uint64_t identity) {
  const auto found = completion_endpoints_.find(identity);
  FutureCompletionEndpoint::Close(*found->second);
  completion_endpoints_.erase(found);
}

void FutureScheduler::AppendExternalResources(std::vector<ExternalResourceInfo>& result) const {
  external_resources_.AppendTo(result);
  const auto accesses = host_accesses_.load(std::memory_order_acquire);
  if (accesses != 0) {
    result.push_back(
        {.kind = ExternalResourceKind::HostAccess, .identity = 0, .references = accesses});
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

void FutureScheduler::PostMicrotask(absl::AnyInvocable<void()> task) {
  if (IsOwnerThread()) {
    microtasks_.push_back({.context = context_, .action = std::move(task)});
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

void FutureScheduler::NotifyUnhandledError(const Future& future, ::vela::runtime::Value error) {
  if (unhandled_error_handler_) {
    unhandled_error_handler_(future, error);
  }
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
  return ScheduleTimer(delay, std::chrono::milliseconds(0), std::move(callback));
}

Timer FutureScheduler::Periodic(std::chrono::milliseconds interval,
                                std::function<void()> callback) {
  return ScheduleTimer(interval, interval, std::move(callback));
}

Timer FutureScheduler::ScheduleTimer(std::chrono::milliseconds delay,
                                     std::chrono::milliseconds interval,
                                     std::function<void()> callback) {
  assert(IsOwnerThread());
  auto state = std::make_shared<Timer::State>();
  state->callback = std::move(callback);
  state->scheduler = this;
  state->context = context_;
  PushEvent(Event{
      .deadline = Clock::now() + delay,
      .sequence = next_sequence_++,
      .interval = interval,
      .state = state,
  });
  return Timer(std::move(state));
}

void FutureScheduler::DrainMicrotasks() {
  while (!microtasks_.empty() || !checkpoints_.empty()) {
    if (!microtasks_.empty()) {
      auto task = std::move(microtasks_.front());
      microtasks_.pop_front();
      const ExecutionContextScope scope(*this, task.context);
      task.action();
    } else {
      auto checkpoint = std::move(checkpoints_.front());
      checkpoints_.pop_front();
      checkpoint();
    }
  }
}

FuturePumpStep FutureScheduler::PumpOne(FuturePumpMode mode) {
  assert(IsOwnerThread());
  const ExternalResource execution(external_resources_, ExternalResourceKind::Execution);
  while (true) {
    if (!microtasks_.empty() || !checkpoints_.empty()) {
      DrainMicrotasks();
      return FuturePumpStep::Executed;
    }
    ReceiveMessages();
    if (!host_events_.empty()) {
      auto event = std::move(host_events_.front());
      host_events_.pop_front();
      const ExecutionContextScope scope(*this, std::nullopt);
      event();
      DrainMicrotasks();
      return FuturePumpStep::Executed;
    }
    while (!events_.empty() && events_.front().state->cancelled.load()) {
      const auto event = RemoveEvent(0);
      Timer::Close(*event.state);
    }
    if (events_.empty()) {
      if (mode != FuturePumpMode::WaitForHost || host_sources_.load() == 0) {
        return FuturePumpStep::Idle;
      }
      wakeup_.acquire();
      continue;
    }
    const auto deadline = events_.front().deadline;
    if (deadline > Clock::now()) {
      if (mode == FuturePumpMode::ReadyWork) {
        return FuturePumpStep::Idle;
      }
      std::ignore = wakeup_.try_acquire_until(deadline);
      continue;
    }
    Fire(RemoveEvent(0));
    return FuturePumpStep::Executed;
  }
}

void FutureScheduler::Fire(Event event) {
  auto& state = *event.state;
  auto callback = std::move(state.callback);
  if (callback && !state.cancelled.load()) {
    const ExecutionContextScope scope(*this, state.context);
    callback();
    DrainMicrotasks();
  }
  if (event.interval.count() > 0 && !state.cancelled.load()) {
    state.callback = std::move(callback);
    event.deadline = Clock::now() + event.interval;
    event.sequence = next_sequence_++;
    PushEvent(std::move(event));
  } else {
    Timer::Close(state);
  }
}

void FutureScheduler::PushEvent(Event event) {
  const auto index = events_.size();
  event.state->event_index = index;
  events_.push_back(std::move(event));
  ReorderEvent(index);
}

FutureScheduler::Event FutureScheduler::RemoveEvent(std::size_t index) {
  auto event = std::move(events_[index]);
  const auto last = events_.size() - 1;
  if (index != last) {
    events_[index] = std::move(events_.back());
    events_[index].state->event_index = index;
  }
  events_.pop_back();
  event.state->event_index = Timer::State::kNotQueued;
  if (index < events_.size()) {
    ReorderEvent(index);
  }
  return event;
}

void FutureScheduler::SwapEvents(std::size_t first, std::size_t second) noexcept {
  auto& first_event = events_[first];
  auto& second_event = events_[second];
  std::swap(first_event, second_event);
  first_event.state->event_index = first;
  second_event.state->event_index = second;
}

void FutureScheduler::ReorderEvent(std::size_t index) noexcept {
  const EventOrder order;
  while (index != 0) {
    const auto parent = (index - 1) / 2;
    if (!order(events_[parent], events_[index])) {
      break;
    }
    SwapEvents(parent, index);
    index = parent;
  }
  const auto size = events_.size();
  while (index < size / 2) {
    auto child = (index * 2) + 1;
    if (child + 1 < size && order(events_[child], events_[child + 1])) {
      ++child;
    }
    if (!order(events_[index], events_[child])) {
      break;
    }
    SwapEvents(index, child);
    index = child;
  }
}

bool FutureScheduler::IsOwnerThread() const noexcept {
  return owner_ == std::this_thread::get_id();
}

bool FutureScheduler::CanSchedule(std::chrono::milliseconds duration) noexcept {
  return duration.count() >= 0 && duration <= std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  Clock::time_point::max() - Clock::now());
}

FuturePumpResult FutureScheduler::PumpUntil(Future& root, FuturePumpMode mode) {
  while (root.is_pending()) {
    if (PumpOne(mode) == FuturePumpStep::Idle) {
      return FuturePumpResult::Idle;
    }
  }
  return FuturePumpResult::Completed;
}

}  // namespace vela::runtime
