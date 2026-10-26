#include "runtime/future.h"
#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/value.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace vela::runtime {
namespace {

void ConsumeCompletion(std::expected<void, FutureCompletionError> completion) {
  if (!completion.has_value()) {
    std::unreachable();
  }
}

}  // namespace

ExecutionStatus FutureCompletionStatus(FutureCompletionError error) noexcept {
  switch (error) {
    case FutureCompletionError::AlreadyCompleted:
      return ExecutionStatus::FutureAlreadyCompleted;
    case FutureCompletionError::SelfAdoption:
      return ExecutionStatus::FutureSelfAdoption;
    case FutureCompletionError::DifferentIsolate:
      return ExecutionStatus::InvalidIsolate;
  }
  std::unreachable();
}

struct Future::SharedState final {
  SharedState() = default;
  SharedState(const SharedState&) = delete;
  SharedState& operator=(const SharedState&) = delete;
  SharedState(SharedState&&) = delete;
  SharedState& operator=(SharedState&&) = delete;
  struct Waiter final {
    std::function<void(const FutureResult&)> callback;
    std::shared_ptr<std::vector<::vela::runtime::Value>> references;
  };
  std::atomic_size_t external_count{0};
  std::atomic_bool completion_requested{false};
  FutureState state = FutureState::Pending;
  ::vela::runtime::Value result = Value::Null();
  std::optional<FutureDiagnostic> diagnostic;
  bool error_observed = false;
  bool unhandled_reported = false;
  bool handled_reported = false;
  std::vector<Waiter> waiters;
  FutureScheduler* scheduler = nullptr;
  Heap* heap = nullptr;
  std::thread::id owner = std::this_thread::get_id();
  std::atomic<FutureResult*> incoming{nullptr};
  ~SharedState() { const std::unique_ptr<FutureResult> owned(incoming.load()); }
  void RequireOwner() const {
    assert(scheduler != nullptr ? scheduler->IsOwnerThread() : owner == std::this_thread::get_id());
  }
  std::shared_ptr<std::vector<::vela::runtime::Value>> retained;
  std::weak_ptr<SharedState> adopted;
};

Future::Future(FutureScheduler* scheduler) : state_(std::make_shared<SharedState>()) {
  state_->scheduler = scheduler;
  ++state_->external_count;
}

Future::Future(std::shared_ptr<SharedState> state, Rooting rooting)
    : state_(std::move(state)), external_(rooting == Rooting::External) {
  if (external_) {
    ++state_->external_count;
  }
}

Future::~Future() {
  Release();
}

void Future::Release() noexcept {
  if (state_ != nullptr && external_) {
    --state_->external_count;
  }
  if (state_ != nullptr && state_->scheduler != nullptr && !state_->scheduler->IsOwnerThread()) {
    auto* scheduler = state_->scheduler;
    scheduler->PostMicrotask([state = std::move(state_)] {});
  }
}

Future::Future(const Future& other)
    : Future(other.state_, other.external_ ? Rooting::External : Rooting::Traced) {}

Future& Future::operator=(const Future& other) {
  if (this != &other) {
    Release();
    state_ = other.state_;
    external_ = other.external_;
    if (external_) {
      ++state_->external_count;
    }
  }
  return *this;
}

Future::Future(Future&& other) noexcept
    : state_(std::move(other.state_)), external_(other.external_) {}

Future& Future::operator=(Future&& other) noexcept {
  if (this != &other) {
    Release();
    state_ = std::move(other.state_);
    external_ = other.external_;
  }
  return *this;
}

Future Future::Internal() const {
  return Future(state_, Rooting::Traced);
}
Future Future::Rooted() const {
  return Future(state_, Rooting::External);
}

void Future::MakeHeapOwned() noexcept {
  if (external_) {
    --state_->external_count;
    external_ = false;
  }
}

void Future::KeepAlive(std::shared_ptr<std::vector<::vela::runtime::Value>> references) {
  state_->RequireOwner();
  state_->retained = std::move(references);
  if (auto* heap = state_->heap; heap != nullptr && heap->IsCollecting()) {
    Trace(*heap);
    if (heap->phase() == GcPhase::Sweep) {
      heap->DrainGray();
    }
  }
}

void Future::Root::Trace(Heap& heap) const noexcept {
  if (const auto state = state_.lock(); state != nullptr && state->external_count != 0) {
    Future(state, Rooting::Traced).Trace(heap);
  }
}

Future Future::Value(::vela::runtime::Value value, FutureScheduler* scheduler) {
  Future future(scheduler);
  ConsumeCompletion(future.Complete(value));
  return future;
}

Future Future::Error(::vela::runtime::Value error, FutureScheduler* scheduler) {
  Future future(scheduler);
  ConsumeCompletion(future.CompleteError(error));
  return future;
}

void Future::Trace(Heap& heap) const noexcept {
  if (state_->state != FutureState::Pending) {
    heap.MarkValue(state_->result);
  }
  if (const auto* incoming = state_->incoming.load(std::memory_order_acquire)) {
    heap.MarkValue(incoming->value);
  }
  if (state_->retained != nullptr) {
    for (const auto value : *state_->retained) {
      heap.MarkValue(value);
    }
  }
  for (const auto& waiter : state_->waiters) {
    if (waiter.references != nullptr) {
      for (const auto value : *waiter.references) {
        heap.MarkValue(value);
      }
    }
  }
  if (const auto adopted = state_->adopted.lock()) {
    Future(adopted, Rooting::Traced).Trace(heap);
  }
}

void Future::AttachHeap(Heap& heap) {
  state_->RequireOwner();
  if (state_->heap == nullptr) {
    state_->heap = &heap;
    heap.future_roots_.push_back(Root(state_));
    if (heap.IsCollecting()) {
      Trace(heap);
      if (heap.phase() == GcPhase::Sweep) {
        heap.DrainGray();
      }
    }
  }
}

std::expected<void, FutureBindingError> Future::Bind(FutureScheduler& scheduler, Heap& heap) {
  if ((state_->scheduler != nullptr && state_->scheduler != &scheduler) ||
      (state_->heap != nullptr && state_->heap != &heap)) {
    return std::unexpected(FutureBindingError::DifferentIsolate);
  }
  const auto newly_bound = state_->scheduler == nullptr;
  state_->scheduler = &scheduler;
  AttachHeap(heap);
  if (newly_bound && state_->state == FutureState::Failed && !state_->error_observed) {
    const auto self = Rooted();
    scheduler.checkpoints_.emplace_back([self] { self.ReportUnhandledError(); });
  }
  return {};
}

Future Future::Delayed(std::chrono::milliseconds delay,
                       FutureScheduler& scheduler,
                       std::function<Future()> computation) {
  Future result(&scheduler);
  static_cast<void>(
      scheduler.After(delay, [result, computation = std::move(computation)]() mutable {
        if (computation) {
          result.AdoptComputation(computation());
        } else {
          ConsumeCompletion(result.Complete(::vela::runtime::Value::Null()));
        }
      }));
  return result;
}

Future Future::Sleep(std::chrono::milliseconds delay, FutureScheduler& scheduler) {
  return Delayed(delay, scheduler);
}

Future Future::Wait(std::span<const Future> values,
                    FutureScheduler& scheduler,
                    FutureWaitMode mode) {
  Future result(&scheduler);
  if (values.empty()) {
    ConsumeCompletion(result.Complete(::vela::runtime::Value::Null()));
    return result;
  }
  struct Aggregate final {
    std::size_t remaining;
    std::optional<FutureResult> failure;
  };
  auto aggregate =
      std::make_shared<Aggregate>(Aggregate{.remaining = values.size(), .failure = {}});
  auto references = std::make_shared<std::vector<::vela::runtime::Value>>(1);
  result.KeepAlive(references);
  for (const auto& source : values) {
    source.ObserveResult(
        [target = result.Internal(), aggregate, references,
         mode](const FutureResult& input) mutable {
          if (input.state == FutureState::Failed && !aggregate->failure) {
            aggregate->failure = input;
            references->front() = input.value;
            if (mode == FutureWaitMode::EagerError) {
              ConsumeCompletion(target.CompleteError(input.value, input.diagnostic));
            }
          }
          if (--aggregate->remaining == 0 && target.is_pending()) {
            if (aggregate->failure) {
              ConsumeCompletion(
                  target.CompleteError(aggregate->failure->value, aggregate->failure->diagnostic));
            } else {
              ConsumeCompletion(target.Complete(::vela::runtime::Value::Null()));
            }
          }
        },
        references);
  }
  return result;
}

Future Future::Race(std::span<const Future> values, FutureScheduler& scheduler) {
  Future result(&scheduler);
  for (const auto& source : values) {
    source.ObserveResult([target = result.Internal()](const FutureResult& input) mutable {
      if (target.is_pending()) {
        ConsumeCompletion(input.state == FutureState::Succeeded
                              ? target.Complete(input.value)
                              : target.CompleteError(input.value, input.diagnostic));
      }
    });
  }
  return result;
}

FutureState Future::state() const noexcept {
  state_->RequireOwner();
  return state_->state;
}

bool Future::IsCompleted() const noexcept {
  return state_->completion_requested;
}

std::optional<::vela::runtime::Value> Future::value() const {
  state_->RequireOwner();
  return state_->state == FutureState::Succeeded ? std::optional(state_->result) : std::nullopt;
}

std::optional<::vela::runtime::Value> Future::error() const {
  state_->RequireOwner();
  return state_->state == FutureState::Failed ? std::optional(state_->result) : std::nullopt;
}

std::optional<FutureDiagnostic> Future::error_diagnostic() const {
  state_->RequireOwner();
  return state_->state == FutureState::Failed ? state_->diagnostic : std::nullopt;
}

void Future::Observe(Callback callback) const {
  ObserveResult([callback = std::move(callback)](const FutureResult& result) {
    callback(result.state, result.value);
  });
}

void Future::ObserveResult(std::function<void(const FutureResult&)> callback,
                           std::shared_ptr<std::vector<::vela::runtime::Value>> references) const {
  state_->RequireOwner();
  std::optional<FutureResult> completed;
  bool notify_handled = false;
  {
    if (state_->state == FutureState::Pending) {
      state_->waiters.push_back(
          {.callback = std::move(callback), .references = std::move(references)});
      return;
    }
    if (state_->state == FutureState::Failed) {
      state_->error_observed = true;
      notify_handled = state_->unhandled_reported && !state_->handled_reported;
      state_->handled_reported = state_->handled_reported || notify_handled;
    }
    completed = FutureResult{
        .state = state_->state,
        .value = state_->result,
        .diagnostic = state_->diagnostic.value_or(FutureDiagnostic{}),
    };
  }
  auto roots =
      state_->heap != nullptr && references != nullptr
          ? std::make_shared<RootRange>(*state_->heap, references->data(), references->size())
          : nullptr;
  if (state_->scheduler != nullptr) {
    const Future keep_alive(state_, Rooting::External);
    state_->scheduler->PostMicrotask([callback = std::move(callback), result = *completed,
                                      keep_alive, references = std::move(references),
                                      roots = std::move(roots)]() mutable { callback(result); });
    if (notify_handled) {
      const auto self = *this;
      state_->scheduler->PostMicrotask([self]() mutable { self.ReportHandledError(); });
    }
  } else {
    callback(*completed);
    if (notify_handled) {
      ReportHandledError();
    }
  }
}

std::expected<void, FutureCompletionError> Future::Complete(::vela::runtime::Value value) {
  if ((state_->scheduler == nullptr || state_->scheduler->IsOwnerThread()) && value.IsObject()) {
    if (const auto* source = Heap::GetFuture(*value.AsObject())) {
      return Adopt(*source);
    }
  }
  return Settle({.state = FutureState::Succeeded, .value = value, .diagnostic = {}});
}

std::expected<void, FutureCompletionError> Future::CompleteError(::vela::runtime::Value error) {
  return CompleteError(error, {});
}

std::expected<void, FutureCompletionError> Future::CompleteError(::vela::runtime::Value error,
                                                                 FutureDiagnostic diagnostic) {
  return Settle(
      {.state = FutureState::Failed, .value = error, .diagnostic = std::move(diagnostic)});
}

std::expected<void, FutureCompletionError> Future::Settle(FutureResult result) {
  if (state_->scheduler == nullptr) {
    state_->RequireOwner();
  }
  if (state_->completion_requested.exchange(true, std::memory_order_acq_rel)) {
    return std::unexpected(FutureCompletionError::AlreadyCompleted);
  }
  if (state_->scheduler != nullptr && !state_->scheduler->IsOwnerThread()) {
    state_->incoming.store(std::make_unique<FutureResult>(std::move(result)).release(),
                           std::memory_order_release);
    Future self(state_, Rooting::External);
    state_->scheduler->PostMicrotask([self = std::move(self)]() mutable {
      std::unique_ptr<FutureResult> incoming(self.state_->incoming.exchange(nullptr));
      self.Publish(*incoming);
    });
    return {};
  }
  Publish(result);
  return {};
}

void Future::Publish(const FutureResult& result) {
  state_->RequireOwner();
  if (result.state == FutureState::Succeeded && result.value.IsObject()) {
    if (const auto* source = Heap::GetFuture(*result.value.AsObject())) {
      if (CanAdopt(*source)) {
        Follow(*source);
      } else {
        Publish({
            .state = FutureState::Failed,
            .value = ::vela::runtime::Value::Null(),
            .diagnostic =
                {
                    .message = "Future cannot adopt itself",
                    .stack_trace = {},
                    .status = ExecutionStatus::FutureSelfAdoption,
                },
        });
      }
      return;
    }
  }
  std::vector<SharedState::Waiter> waiters;
  bool report_unhandled = false;
  {
    state_->state = result.state;
    state_->result = result.value;
    state_->diagnostic = result.diagnostic;
    state_->error_observed = result.state != FutureState::Failed || !state_->waiters.empty();
    report_unhandled = !state_->error_observed;
    waiters = std::move(state_->waiters);
    state_->retained.reset();
    state_->adopted.reset();
  }
  if (auto* heap = state_->heap; heap != nullptr && heap->IsCollecting()) {
    heap->MarkValue(result.value);
    if (heap->phase() == GcPhase::Sweep) {
      heap->DrainGray();
    }
  }
  for (auto& waiter : waiters) {
    auto roots = state_->heap != nullptr && waiter.references != nullptr
                     ? std::make_shared<RootRange>(*state_->heap, waiter.references->data(),
                                                   waiter.references->size())
                     : nullptr;
    if (state_->scheduler != nullptr) {
      const Future keep_alive(state_, Rooting::External);
      state_->scheduler->PostMicrotask(
          [waiter = std::move(waiter), result, keep_alive, roots = std::move(roots)]() mutable {
            waiter.callback(result);
          });
    } else {
      waiter.callback(result);
    }
  }
  if (state_->scheduler != nullptr && report_unhandled) {
    const Future self(state_, Rooting::External);
    state_->scheduler->checkpoints_.emplace_back([self]() mutable { self.ReportUnhandledError(); });
  }
}

std::expected<void, FutureCompletionError> Future::Adopt(const Future& source) {
  state_->RequireOwner();
  if ((state_->scheduler != nullptr && source.state_->scheduler != nullptr &&
       state_->scheduler != source.state_->scheduler) ||
      (state_->heap != nullptr && source.state_->heap != nullptr &&
       state_->heap != source.state_->heap)) {
    return std::unexpected(FutureCompletionError::DifferentIsolate);
  }
  if (!CanAdopt(source)) {
    return std::unexpected(FutureCompletionError::SelfAdoption);
  }
  if (state_->completion_requested.exchange(true, std::memory_order_acq_rel)) {
    return std::unexpected(FutureCompletionError::AlreadyCompleted);
  }
  Follow(source);
  return {};
}

bool Future::CanAdopt(const Future& source) const {
  auto dependency = source.state_;
  while (dependency != nullptr) {
    if (state_ == dependency) {
      return false;
    }
    dependency = dependency->adopted.lock();
  }
  return true;
}

void Future::Follow(const Future& source) {
  state_->adopted = source.state_;
  state_->retained.reset();
  auto target = Internal();
  source.ObserveResult([target](const FutureResult& result) mutable { target.Publish(result); });
}

void Future::AdoptComputation(const Future& source) {
  const auto adopted = Adopt(source);
  if (!adopted && adopted.error() != FutureCompletionError::AlreadyCompleted) {
    const auto status = adopted.error() == FutureCompletionError::SelfAdoption
                            ? ExecutionStatus::FutureSelfAdoption
                            : ExecutionStatus::InvalidIsolate;
    ConsumeCompletion(CompleteError(::vela::runtime::Value::Null(),
                                    {
                                        .message = std::string(ExecutionStatusName(status)),
                                        .stack_trace = {},
                                        .status = status,
                                    }));
  }
}

void Future::ReportUnhandledError() const {
  FutureScheduler* scheduler = nullptr;
  ::vela::runtime::Value error = Value::Null();
  {
    if (state_->state != FutureState::Failed || state_->error_observed ||
        state_->unhandled_reported) {
      return;
    }
    state_->unhandled_reported = true;
    scheduler = state_->scheduler;
    error = state_->result;
  }
  if (scheduler != nullptr) {
    scheduler->NotifyUnhandledError(*this, error);
  }
}

void Future::ReportHandledError() const {
  FutureScheduler* scheduler = nullptr;
  ::vela::runtime::Value error = Value::Null();
  {
    if (state_->state != FutureState::Failed || !state_->error_observed ||
        !state_->unhandled_reported) {
      return;
    }
    scheduler = state_->scheduler;
    error = state_->result;
  }
  if (scheduler != nullptr) {
    scheduler->NotifyHandledError(*this, error);
  }
}

Future Future::Then(Continuation on_value, Continuation on_error) const {
  state_->RequireOwner();
  Future result(state_->scheduler);
  result.state_->adopted = state_;
  ObserveResult([target = result.Internal(), on_value = std::move(on_value),
                 on_error = std::move(on_error)](const FutureResult& input) mutable {
    auto& callback = input.state == FutureState::Succeeded ? on_value : on_error;
    if (callback) {
      target.AdoptComputation(callback(input.value));
    } else {
      ConsumeCompletion(input.state == FutureState::Succeeded
                            ? target.Complete(input.value)
                            : target.CompleteError(input.value, input.diagnostic));
    }
  });
  return result;
}

Future Future::CatchError(Continuation on_error) const {
  return Then({}, std::move(on_error));
}

Future Future::WhenComplete(const std::function<void()>& action) const {
  state_->RequireOwner();
  Future result(state_->scheduler);
  result.state_->adopted = state_;
  ObserveResult([target = result.Internal(), action](const FutureResult& input) mutable {
    action();
    ConsumeCompletion(input.state == FutureState::Succeeded
                          ? target.Complete(input.value)
                          : target.CompleteError(input.value, input.diagnostic));
  });
  return result;
}

Future Future::Timeout(std::chrono::milliseconds duration,
                       FutureScheduler& scheduler,
                       Continuation on_timeout) const {
  Future result(&scheduler);
  result.state_->adopted = state_;
  auto settled = std::make_shared<bool>(false);
  auto timer =
      scheduler.After(duration, [result, settled, on_timeout = std::move(on_timeout)]() mutable {
        if (std::exchange(*settled, true)) {
          return;
        }
        if (on_timeout) {
          result.AdoptComputation(on_timeout(::vela::runtime::Value::Null()));
        } else {
          ConsumeCompletion(result.CompleteError(::vela::runtime::Value::Null()));
        }
      });
  ObserveResult([target = result.Internal(), settled, timer](const FutureResult& input) mutable {
    if (!std::exchange(*settled, true)) {
      timer.Cancel();
      ConsumeCompletion(input.state == FutureState::Succeeded
                            ? target.Complete(input.value)
                            : target.CompleteError(input.value, input.diagnostic));
    }
  });
  return result;
}

std::expected<void, FutureCompletionError> Completer::Complete(::vela::runtime::Value value) {
  const auto result = future_.Complete(value);
  if (result && producer_ != nullptr) {
    producer_->Release();
  }
  return result;
}
std::expected<void, FutureCompletionError> Completer::CompleteError(::vela::runtime::Value error) {
  return CompleteError(error, {});
}
std::expected<void, FutureCompletionError> Completer::CompleteError(::vela::runtime::Value error,
                                                                    FutureDiagnostic diagnostic) {
  const auto result = future_.CompleteError(error, std::move(diagnostic));
  if (result && producer_ != nullptr) {
    producer_->Release();
  }
  return result;
}

struct CancellationToken::State final {
  State() = default;
  State(const State&) = delete;
  State& operator=(const State&) = delete;
  State(State&&) = delete;
  State& operator=(State&&) = delete;
  struct Callback final {
    Callback* next = nullptr;
    std::function<void()> action;
  };
  static Callback* Cancelled() {
    static Callback sentinel;
    return &sentinel;
  }
  std::atomic<Callback*> callbacks{nullptr};
  ~State() {
    auto* callback = callbacks.load();
    while (callback != nullptr && callback != Cancelled()) {
      const std::unique_ptr<Callback> owned(callback);
      auto* next = owned->next;
      callback = next;
    }
  }
};

CancellationToken::CancellationToken() : state_(std::make_shared<State>()) {}

void CancellationToken::Cancel() noexcept {
  auto* callback = state_->callbacks.exchange(State::Cancelled(), std::memory_order_acq_rel);
  if (callback == State::Cancelled()) {
    return;
  }
  // CAS 链按逆序发布；取消时恢复注册顺序，回调在锁外且只执行一次。
  State::Callback* ordered = nullptr;
  while (callback != nullptr) {
    auto* next = callback->next;
    callback->next = ordered;
    ordered = callback;
    callback = next;
  }
  while (ordered != nullptr) {
    std::unique_ptr<State::Callback> current(ordered);
    ordered = ordered->next;
    current->action();
  }
}

bool CancellationToken::IsCancelled() const noexcept {
  return state_->callbacks.load(std::memory_order_acquire) == State::Cancelled();
}

void CancellationToken::Observe(std::function<void()> callback) {
  auto node = std::make_unique<State::Callback>();
  node->action = std::move(callback);
  auto* head = state_->callbacks.load(std::memory_order_acquire);
  while (true) {
    if (head == State::Cancelled()) {
      node->action();
      return;
    }
    node->next = head;
    if (state_->callbacks.compare_exchange_weak(head, node.get(), std::memory_order_release,
                                                std::memory_order_acquire)) {
      break;
    }
  }
  std::ignore = node.release();
}

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
