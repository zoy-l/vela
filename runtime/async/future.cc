#include "runtime/async/future.h"
#include "base/execution_status.h"
#include "runtime/async/execution_context.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/intrusive_link_internal.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/value/value.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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
    case FutureCompletionError::Closed:
      return ExecutionStatus::ExecutionStopped;
  }
  std::unreachable();
}

struct Future::SharedState final {
  SharedState() = default;
  ~SharedState() = default;
  SharedState(const SharedState&) = delete;
  SharedState& operator=(const SharedState&) = delete;
  SharedState(SharedState&&) = delete;
  SharedState& operator=(SharedState&&) = delete;
  struct Waiter final {
    std::function<void(const FutureResult&)> callback;
    std::shared_ptr<std::vector<::vela::runtime::Value>> references;
    std::optional<ExecutionContext> context;
  };
  ExternalResource external;
  std::atomic_size_t retained_count{0};
  std::atomic_bool completion_requested{false};
  FutureResult completion{.value = Value::Null(), .diagnostic = {}};
  enum class ErrorObservation : std::uint8_t { Unobserved, ReportedUnhandled, Observed };
  ErrorObservation error_observation = ErrorObservation::Unobserved;
  std::vector<Waiter> waiters;
  FutureScheduler* scheduler = nullptr;
  std::optional<ExecutionContext> creation_context;
  Heap* heap = nullptr;
  Root root{*this};
  std::shared_ptr<std::vector<::vela::runtime::Value>> retained;
  std::weak_ptr<SharedState> adopted;
};

// 微任务独占监听器：回调、稳定根及结果句柄共用一次分配，取消排队也能立即释放根。
struct Future::Observer final {
  Observer(Future source,
           std::function<void(const FutureResult&)> action,
           std::shared_ptr<std::vector<::vela::runtime::Value>> values)
      : future(std::move(source)), callback(std::move(action)), references(std::move(values)) {
    if (auto* heap = future.state_->heap; heap != nullptr && references != nullptr) {
      roots.Attach(*heap, references->data(), references->size());
    }
  }

  void Invoke() const { callback(future.state_->completion); }

  Future future;
  std::function<void(const FutureResult&)> callback;
  std::shared_ptr<std::vector<::vela::runtime::Value>> references;
  RootRange roots{RootOwner::Runtime};
};

Future::Future(FutureScheduler* scheduler)
    : state_(new SharedState, [](SharedState* state) {
        // 只有最后一个句柄在宿主线程释放时才转移销毁，捕获资源始终在所属线程析构。
        auto* owner = state->scheduler;
        if (owner != nullptr && !owner->IsOwnerThread()) {
          owner->PostMicrotask([owned = std::unique_ptr<SharedState>(state)] {});
        } else {
          const std::unique_ptr<SharedState> owned(state);
        }
      }) {
  state_->scheduler = scheduler;
  if (scheduler != nullptr) {
    state_->creation_context = scheduler->context();
    state_->external.Attach(scheduler->external_resources(), ExternalResourceKind::Future);
  }
  Retain();
}

Future::Future(std::shared_ptr<SharedState> state, Rooting rooting)
    : state_(std::move(state)), rooting_(rooting) {
  Retain();
}

Future::~Future() {
  Release();
}

void Future::Release() noexcept {
  if (state_ == nullptr) {
    return;
  }
  auto* scheduler = state_->scheduler;
  const auto host_thread = scheduler != nullptr && !scheduler->IsOwnerThread();
  // 计数归零与最后一个 shared_ptr 的延迟销毁投递之间仍会借用调度器。
  // Reload 读取句柄计数后再读取 host_accesses，不能越过这段宿主析构。
  if (host_thread) {
    scheduler->host_accesses_.fetch_add(1, std::memory_order_relaxed);
  }
  Unroot();
  state_.reset();
  if (host_thread) {
    scheduler->wakeup_.release();
    scheduler->host_accesses_.fetch_sub(1, std::memory_order_release);
  }
}

void Future::Unroot() noexcept {
  if (rooting_ == Rooting::External) {
    state_->external.Release();
  } else if (rooting_ == Rooting::Retained) {
    --state_->retained_count;
  }
}

void Future::Retain() noexcept {
  if (rooting_ == Rooting::External) {
    state_->external.Retain();
  } else if (rooting_ == Rooting::Retained) {
    ++state_->retained_count;
  }
}

Future::Future(const Future& other) : Future(other.state_, other.rooting_) {}

Future& Future::operator=(const Future& other) {
  if (this != &other) {
    Release();
    state_ = other.state_;
    rooting_ = other.rooting_;
    Retain();
  }
  return *this;
}

Future::Future(Future&& other) noexcept
    : state_(std::move(other.state_)), rooting_(other.rooting_) {}

Future& Future::operator=(Future&& other) noexcept {
  if (this != &other) {
    Release();
    state_ = std::move(other.state_);
    rooting_ = other.rooting_;
  }
  return *this;
}

Future Future::Internal() const {
  return Future(state_, Rooting::Traced);
}
Future Future::Rooted() const {
  return Future(state_, Rooting::External);
}

Future Future::Retained() const {
  return Future(state_, Rooting::Retained);
}

void Future::KeepAlive(std::shared_ptr<std::vector<::vela::runtime::Value>> references) {
  state_->retained = std::move(references);
  if (auto* heap = state_->heap; heap != nullptr) {
    heap->WriteRootBarrier(*this);
  }
}

Future::Root::~Root() {
  internal::UnlinkIntrusive(*this, &Root::next_, &Root::previous_link_);
}

void Future::Root::Attach(Heap& heap) noexcept {
  internal::PrependIntrusive(*this, heap.future_roots_, &Root::next_, &Root::previous_link_);
}

void Future::Root::Trace(Heap& heap) const noexcept {
  if (state_->external.references() != 0 || state_->retained_count != 0) {
    TraceState(*state_, heap);
  }
}

Future Future::Value(::vela::runtime::Value value, FutureScheduler* scheduler) {
  Future future(scheduler);
  if (const auto completion = future.Complete(value); !completion) {
    const auto status = FutureCompletionStatus(completion.error());
    ConsumeCompletion(future.CompleteError(::vela::runtime::Value::Null(),
                                           {
                                               .message = std::string(ExecutionStatusName(status)),
                                               .stack_trace = {},
                                               .status = status,
                                               .origin = {},
                                           }));
  }
  return future;
}

Future Future::Error(::vela::runtime::Value error, FutureScheduler* scheduler) {
  Future future(scheduler);
  ConsumeCompletion(future.CompleteError(error));
  return future;
}

void Future::Trace(Heap& heap) const noexcept {
  TraceState(*state_, heap);
}

void Future::TraceState(const SharedState& initial, Heap& heap) noexcept {
  const auto* state = &initial;
  std::shared_ptr<SharedState> adopted;
  while (state != nullptr) {
    if (state->completion.state != FutureState::Pending) {
      heap.MarkValue(state->completion.value);
    }
    if (state->retained != nullptr) {
      for (const auto value : *state->retained) {
        heap.MarkValue(value);
      }
    }
    for (const auto& waiter : state->waiters) {
      if (waiter.references != nullptr) {
        for (const auto value : *waiter.references) {
          heap.MarkValue(value);
        }
      }
    }
    adopted = state->adopted.lock();
    state = adopted.get();
  }
}

void Future::AttachHeap(Heap& heap) {
  if (state_->heap == nullptr) {
    state_->heap = &heap;
    state_->root.Attach(heap);
    heap.WriteRootBarrier(*this);
  }
}

std::expected<void, ExecutionStatus> Future::Bind(FutureScheduler& scheduler, Heap& heap) {
  if ((state_->scheduler != nullptr && state_->scheduler != &scheduler) ||
      (state_->heap != nullptr && state_->heap != &heap)) {
    return std::unexpected(ExecutionStatus::InvalidIsolate);
  }
  const auto newly_bound = state_->scheduler == nullptr;
  if (newly_bound) {
    state_->creation_context = scheduler.context();
    state_->external.Attach(scheduler.external_resources(), ExternalResourceKind::Future);
  }
  state_->scheduler = &scheduler;
  if (newly_bound && state_->completion.state == FutureState::Failed &&
      state_->completion.diagnostic.origin.identity == 0) {
    state_->completion.diagnostic.origin.identity = scheduler.NewErrorIdentity();
  }
  AttachHeap(heap);
  if (newly_bound && state_->completion.state == FutureState::Failed &&
      state_->error_observation == SharedState::ErrorObservation::Unobserved) {
    const auto self = Retained();
    scheduler.checkpoints_.emplace_back([self] { self.ReportUnhandledError(); });
  }
  return {};
}

Future Future::Sleep(std::chrono::milliseconds delay, FutureScheduler& scheduler) {
  Future result(&scheduler);
  static_cast<void>(scheduler.After(delay, [result = result.Retained()]() mutable {
    ConsumeCompletion(result.Complete(::vela::runtime::Value::Null()));
  }));
  return result;
}

FutureState Future::state() const noexcept {
  return state_->completion.state;
}

const FutureResult& Future::result() const noexcept {
  return state_->completion;
}

bool Future::IsCompleted() const noexcept {
  return state_->completion_requested;
}

std::optional<::vela::runtime::Value> Future::value() const {
  return state_->completion.state == FutureState::Succeeded
             ? std::optional(state_->completion.value)
             : std::nullopt;
}

std::optional<::vela::runtime::Value> Future::error() const {
  return state_->completion.state == FutureState::Failed ? std::optional(state_->completion.value)
                                                         : std::nullopt;
}

std::optional<FutureDiagnostic> Future::error_diagnostic() const {
  return state_->completion.state == FutureState::Failed
             ? std::optional(state_->completion.diagnostic)
             : std::nullopt;
}

void Future::Observe(Callback callback) const {
  ObserveResult([callback = std::move(callback)](const FutureResult& result) {
    callback(result.state, result.value);
  });
}

void Future::MarkObserved() const noexcept {
  state_->error_observation = SharedState::ErrorObservation::Observed;
}

void Future::ObserveResult(std::function<void(const FutureResult&)> callback,
                           std::shared_ptr<std::vector<::vela::runtime::Value>> references) const {
  if (auto* heap = state_->heap; heap != nullptr && references != nullptr) {
    heap->WriteRootBarrier(*references);
  }
  const auto context = state_->scheduler != nullptr ? state_->scheduler->context() : std::nullopt;
  MarkObserved();
  if (state_->completion.state == FutureState::Pending) {
    state_->waiters.push_back(
        {.callback = std::move(callback), .references = std::move(references), .context = context});
    return;
  }
  NotifyObserver(std::move(callback), std::move(references), context);
}

void Future::NotifyObserver(std::function<void(const FutureResult&)> callback,
                            std::shared_ptr<std::vector<::vela::runtime::Value>> references,
                            std::optional<ExecutionContext> context) const {
  auto observer =
      std::make_unique<Observer>(Retained(), std::move(callback), std::move(references));
  if (state_->scheduler != nullptr) {
    state_->scheduler->microtasks_.push_back({
        .context = context,
        .action = [observer = std::move(observer)] { observer->Invoke(); },
    });
  } else {
    observer->Invoke();
  }
}

std::expected<void, FutureCompletionError> Future::Complete(::vela::runtime::Value value) {
  return Settle({.state = FutureState::Succeeded, .value = value, .diagnostic = {}});
}

std::expected<void, FutureCompletionError> Future::CompleteError(::vela::runtime::Value error) {
  return CompleteError(error, {});
}

std::optional<ExecutionContext> Future::creation_context() const noexcept {
  return state_->creation_context;
}

std::expected<void, FutureCompletionError> Future::CompleteError(::vela::runtime::Value error,
                                                                 FutureDiagnostic diagnostic) {
  return Settle(
      {.state = FutureState::Failed, .value = error, .diagnostic = std::move(diagnostic)});
}

std::expected<void, FutureCompletionError> Future::Settle(FutureResult result) {
  assert(result.state != FutureState::Pending);
  if (result.state == FutureState::Succeeded) {
    const auto value = result.value;
    if ((state_->scheduler == nullptr || state_->scheduler->IsOwnerThread()) && value.IsObject()) {
      if (const auto* source = Heap::GetFuture(*value.AsObject())) {
        return Adopt(*source);
      }
    }
    return RequestCompletion(std::move(result));
  }
  auto& diagnostic = result.diagnostic;
  if (!diagnostic.origin.captured) {
    auto* scheduler = state_->scheduler;
    diagnostic.origin.context = scheduler != nullptr && scheduler->IsOwnerThread()
                                    ? scheduler->context()
                                    : state_->creation_context;
    diagnostic.origin.captured = true;
  }
  if (diagnostic.origin.identity == 0 && state_->scheduler != nullptr) {
    diagnostic.origin.identity = state_->scheduler->NewErrorIdentity();
  }
  return RequestCompletion(std::move(result));
}

std::expected<void, FutureCompletionError> Future::RequestCompletion(FutureResult&& result) {
  if (state_->completion_requested.exchange(true, std::memory_order_acq_rel)) {
    return std::unexpected(FutureCompletionError::AlreadyCompleted);
  }
  if (state_->scheduler != nullptr && !state_->scheduler->IsOwnerThread()) {
    Future self(state_, Rooting::Retained);
    state_->scheduler->PostMicrotask(
        [self = std::move(self), result = std::move(result)]() mutable {
          self.Publish(std::move(result));
        });
    return {};
  }
  Publish(std::move(result));
  return {};
}

void Future::Publish(FutureResult result) {
  if (result.state == FutureState::Succeeded && result.value.IsObject()) {
    if (const auto* source = Heap::GetFuture(*result.value.AsObject())) {
      if (const auto checked = CheckAdoption(*source); checked) {
        Follow(*source);
      } else {
        const auto status = FutureCompletionStatus(checked.error());
        Publish({
            .state = FutureState::Failed,
            .value = ::vela::runtime::Value::Null(),
            .diagnostic =
                {
                    .message = std::string(ExecutionStatusName(status)),
                    .stack_trace = {},
                    .status = status,
                    .origin = {.context = state_->creation_context, .captured = true},
                },
        });
      }
      return;
    }
  }
  state_->completion = std::move(result);
  if (state_->completion.state != FutureState::Failed) {
    MarkObserved();
  }
  state_->retained.reset();
  state_->adopted.reset();
  if (auto* heap = state_->heap; heap != nullptr) {
    heap->WriteRootBarrier(std::span(&state_->completion.value, 1));
  }
  auto waiters = std::move(state_->waiters);
  for (auto& waiter : waiters) {
    NotifyObserver(std::move(waiter.callback), std::move(waiter.references), waiter.context);
  }
  if (state_->scheduler != nullptr &&
      state_->error_observation == SharedState::ErrorObservation::Unobserved) {
    const auto self = Retained();
    state_->scheduler->checkpoints_.emplace_back([self] { self.ReportUnhandledError(); });
  }
}

std::expected<void, FutureCompletionError> Future::Adopt(const Future& source) {
  if (const auto checked = CheckAdoption(source); !checked) {
    return checked;
  }
  if (state_->completion_requested.exchange(true, std::memory_order_acq_rel)) {
    return std::unexpected(FutureCompletionError::AlreadyCompleted);
  }
  Follow(source);
  return {};
}

std::expected<void, FutureCompletionError> Future::CheckAdoption(const Future& source) const {
  if ((state_->scheduler != nullptr && source.state_->scheduler != nullptr &&
       state_->scheduler != source.state_->scheduler) ||
      (state_->heap != nullptr && source.state_->heap != nullptr &&
       state_->heap != source.state_->heap)) {
    return std::unexpected(FutureCompletionError::DifferentIsolate);
  }
  auto dependency = source.state_;
  while (dependency != nullptr) {
    if (state_ == dependency) {
      return std::unexpected(FutureCompletionError::SelfAdoption);
    }
    dependency = dependency->adopted.lock();
  }
  return {};
}

void Future::Follow(const Future& source) {
  state_->adopted = source.state_;
  state_->retained.reset();
  auto target = Internal();
  source.ObserveResult([target](const FutureResult& result) mutable { target.Publish(result); });
}

void Future::ReportUnhandledError() const {
  if (state_->completion.state != FutureState::Failed ||
      state_->error_observation != SharedState::ErrorObservation::Unobserved) {
    return;
  }
  state_->error_observation = SharedState::ErrorObservation::ReportedUnhandled;
  if (auto* scheduler = state_->scheduler) {
    scheduler->NotifyUnhandledError(*this, state_->completion.value);
  }
}

std::expected<void, FutureCompletionError> Completer::Complete(::vela::runtime::Value value) {
  return Settle({.state = FutureState::Succeeded, .value = value, .diagnostic = {}});
}

FutureCompletionEndpoint Completer::Detach() && {
  auto* scheduler = future_.state_->scheduler;
  assert(scheduler != nullptr && scheduler->IsOwnerThread());
  future_ = future_.Retained();
  return FutureCompletionEndpoint::Create(*scheduler, std::move(*this));
}
std::expected<void, FutureCompletionError> Completer::CompleteError(::vela::runtime::Value error) {
  return CompleteError(error, {});
}
std::expected<void, FutureCompletionError> Completer::CompleteError(::vela::runtime::Value error,
                                                                    FutureDiagnostic diagnostic) {
  return Settle(
      {.state = FutureState::Failed, .value = error, .diagnostic = std::move(diagnostic)});
}

std::expected<void, FutureCompletionError> Completer::Settle(FutureResult result) {
  const auto completion = future_.Settle(std::move(result));
  if (completion && producer_ != nullptr) {
    producer_->Release();
  }
  return completion;
}

}  // namespace vela::runtime
