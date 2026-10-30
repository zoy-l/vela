#include "runtime/future.h"
#include "base/execution_status.h"
#include "runtime/execution_context.h"
#include "runtime/external_resources.h"
#include "runtime/heap.h"
#include "runtime/value.h"

#include <atomic>
#include <cassert>
#include <chrono>
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
  bool error_observed = false;
  bool unhandled_reported = false;
  bool handled_reported = false;
  std::vector<Waiter> waiters;
  FutureScheduler* scheduler = nullptr;
  std::optional<ExecutionContext> creation_context;
  Heap* heap = nullptr;
  std::atomic<FutureResult*> incoming{nullptr};
  ~SharedState() { const std::unique_ptr<FutureResult> owned(incoming.load()); }
  std::shared_ptr<std::vector<::vela::runtime::Value>> retained;
  std::weak_ptr<SharedState> adopted;
};

// 微任务独占监听器：回调、稳定根及结果句柄共用一次分配，取消排队也能立即释放根。
struct Future::Observer final {
  Observer(Future source,
           std::function<void(const FutureResult&)> action,
           std::shared_ptr<std::vector<::vela::runtime::Value>> values,
           std::optional<ExecutionContext> execution_context)
      : future(std::move(source)),
        callback(std::move(action)),
        references(std::move(values)),
        context(execution_context) {
    if (auto* heap = future.state_->heap; heap != nullptr && references != nullptr) {
      roots.Attach(*heap, references->data(), references->size());
    }
  }

  void Invoke() const {
    std::optional<ExecutionContextScope> scope;
    if (auto* scheduler = future.state_->scheduler) {
      scope.emplace(*scheduler, context);
    }
    callback(future.state_->completion);
  }

  Future future;
  std::function<void(const FutureResult&)> callback;
  std::shared_ptr<std::vector<::vela::runtime::Value>> references;
  RootRange roots{RootOwner::Runtime};
  std::optional<ExecutionContext> context;
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

void Future::MakeHeapOwned() noexcept {
  Unroot();
  rooting_ = Rooting::Traced;
}

void Future::KeepAlive(std::shared_ptr<std::vector<::vela::runtime::Value>> references) {
  state_->retained = std::move(references);
  if (auto* heap = state_->heap; heap != nullptr && heap->IsCollecting()) {
    Trace(*heap);
    if (heap->phase() == GcPhase::Sweep) {
      heap->DrainGray();
    }
  }
}

void Future::Root::Trace(Heap& heap) const noexcept {
  if (const auto state = state_.lock();
      state != nullptr && (state->external.references() != 0 || state->retained_count != 0)) {
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
  if (state_->completion.state != FutureState::Pending) {
    heap.MarkValue(state_->completion.value);
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
  if (newly_bound && state_->completion.state == FutureState::Failed && !state_->error_observed) {
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

void Future::ObserveResult(std::function<void(const FutureResult&)> callback,
                           std::shared_ptr<std::vector<::vela::runtime::Value>> references) const {
  if (auto* heap = state_->heap; heap != nullptr && heap->IsCollecting() && references != nullptr) {
    const RootRange roots(*heap, references->data(), references->size(), RootOwner::Runtime);
  }
  const auto context = state_->scheduler != nullptr ? state_->scheduler->context() : std::nullopt;
  if (state_->completion.state == FutureState::Pending) {
    state_->waiters.push_back(
        {.callback = std::move(callback), .references = std::move(references), .context = context});
    return;
  }
  bool notify_handled = false;
  if (state_->completion.state == FutureState::Failed) {
    state_->error_observed = true;
    notify_handled = state_->unhandled_reported && !state_->handled_reported;
    state_->handled_reported = state_->handled_reported || notify_handled;
  }
  NotifyObserver(std::move(callback), std::move(references), context);
  if (notify_handled) {
    if (state_->scheduler != nullptr) {
      const auto self = Retained();
      state_->scheduler->PostMicrotask([self] { self.ReportHandledError(); });
    } else {
      ReportHandledError();
    }
  }
}

void Future::NotifyObserver(std::function<void(const FutureResult&)> callback,
                            std::shared_ptr<std::vector<::vela::runtime::Value>> references,
                            std::optional<ExecutionContext> context) const {
  auto observer =
      std::make_unique<Observer>(Retained(), std::move(callback), std::move(references), context);
  if (state_->scheduler != nullptr) {
    state_->scheduler->PostMicrotask([observer = std::move(observer)] { observer->Invoke(); });
  } else {
    observer->Invoke();
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

std::optional<ExecutionContext> Future::creation_context() const noexcept {
  return state_->creation_context;
}

std::expected<void, FutureCompletionError> Future::CompleteError(::vela::runtime::Value error,
                                                                 FutureDiagnostic diagnostic) {
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
  return Settle(
      {.state = FutureState::Failed, .value = error, .diagnostic = std::move(diagnostic)});
}

std::expected<void, FutureCompletionError> Future::Settle(FutureResult result) {
  if (state_->completion_requested.exchange(true, std::memory_order_acq_rel)) {
    return std::unexpected(FutureCompletionError::AlreadyCompleted);
  }
  if (state_->scheduler != nullptr && !state_->scheduler->IsOwnerThread()) {
    state_->incoming.store(std::make_unique<FutureResult>(std::move(result)).release(),
                           std::memory_order_release);
    Future self(state_, Rooting::Retained);
    state_->scheduler->PostMicrotask([self = std::move(self)]() mutable {
      std::unique_ptr<FutureResult> incoming(self.state_->incoming.exchange(nullptr));
      self.Publish(std::move(*incoming));
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
  state_->error_observed =
      state_->completion.state != FutureState::Failed || !state_->waiters.empty();
  state_->retained.reset();
  state_->adopted.reset();
  if (auto* heap = state_->heap; heap != nullptr && heap->IsCollecting()) {
    heap->MarkValue(state_->completion.value);
    if (heap->phase() == GcPhase::Sweep) {
      heap->DrainGray();
    }
  }
  auto waiters = std::move(state_->waiters);
  for (auto& waiter : waiters) {
    NotifyObserver(std::move(waiter.callback), std::move(waiter.references), waiter.context);
  }
  if (state_->scheduler != nullptr && !state_->error_observed) {
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
  FutureScheduler* scheduler = nullptr;
  ::vela::runtime::Value error = Value::Null();
  {
    if (state_->completion.state != FutureState::Failed || state_->error_observed ||
        state_->unhandled_reported) {
      return;
    }
    state_->unhandled_reported = true;
    scheduler = state_->scheduler;
    error = state_->completion.value;
  }
  if (scheduler != nullptr) {
    scheduler->NotifyUnhandledError(*this, error);
  }
}

void Future::ReportHandledError() const {
  FutureScheduler* scheduler = nullptr;
  ::vela::runtime::Value error = Value::Null();
  {
    if (state_->completion.state != FutureState::Failed || !state_->error_observed ||
        !state_->unhandled_reported) {
      return;
    }
    scheduler = state_->scheduler;
    error = state_->completion.value;
  }
  if (scheduler != nullptr) {
    scheduler->NotifyHandledError(*this, error);
  }
}

std::expected<void, FutureCompletionError> Completer::Complete(::vela::runtime::Value value) {
  const auto result = future_.Complete(value);
  if (result && producer_ != nullptr) {
    producer_->Release();
  }
  return result;
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
  const auto result = future_.CompleteError(error, std::move(diagnostic));
  if (result && producer_ != nullptr) {
    producer_->Release();
  }
  return result;
}

}  // namespace vela::runtime
