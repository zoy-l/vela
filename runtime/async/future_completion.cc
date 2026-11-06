#include "base/execution_status.h"
#include "runtime/async/future.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "base/utf8.h"
#include "runtime/heap/heap.h"
#include "runtime/value/value.h"

namespace vela::runtime {

struct FutureCompletionEndpoint::State final {
  State(FutureScheduler& owner, std::uint64_t completion_identity, Completer target)
      : scheduler(&owner),
        identity(completion_identity),
        completer(std::move(target)),
        run(owner.run_) {}

  std::expected<void, FutureCompletionError> CheckRequest() const {
    if (completed) {
      return std::unexpected(FutureCompletionError::AlreadyCompleted);
    }
    if (scheduler == nullptr) {
      return std::unexpected(FutureCompletionError::Closed);
    }
    return {};
  }

  void Retire() const {
    auto* owner = scheduler;
    const auto completion_identity = identity;
    owner->PostInRun(
        [owner, completion_identity] { owner->RetireCompletionEndpoint(completion_identity); },
        run);
  }

  std::mutex mutex;
  FutureScheduler* scheduler;
  std::uint64_t identity;
  std::optional<Completer> completer;
  std::shared_ptr<FutureTaskRun> run;
  bool completed = false;
};

// 注册表保活 State，宿主副本只共享 Lease。最后一个宿主句柄释放时把清理送回所属线程。
// Shutdown 与投递共用 State 锁；返回后端点控制状态中不再留下旧运行的指针。
struct FutureCompletionEndpoint::Lease final {
  explicit Lease(std::shared_ptr<State> target) : state(std::move(target)) {}
  ~Lease() {
    const std::scoped_lock lock(state->mutex);
    if (state->scheduler != nullptr && !state->completed) {
      state->Retire();
    }
  }
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;
  Lease(Lease&&) = delete;
  Lease& operator=(Lease&&) = delete;
  std::shared_ptr<State> state;
};

FutureCompletionEndpoint::FutureCompletionEndpoint(std::shared_ptr<Lease> lease)
    : lease_(std::move(lease)) {}

FutureCompletionEndpoint FutureCompletionEndpoint::Create(FutureScheduler& scheduler,
                                                          Completer completer) {
  const auto identity = scheduler.next_completion_identity_++;
  auto state = std::make_shared<State>(scheduler, identity, std::move(completer));
  scheduler.completion_endpoints_.emplace(identity, state);
  return FutureCompletionEndpoint(std::make_shared<Lease>(std::move(state)));
}

void FutureCompletionEndpoint::Close(State& state, CloseReason reason) {
  std::optional<Completer> retired;
  {
    const std::scoped_lock lock(state.mutex);
    retired = std::exchange(state.completer, std::nullopt);
    state.scheduler = nullptr;
    if (reason == CloseReason::Shutdown) {
      state.completed = false;
    }
  }
}

void FutureScheduler::CloseRunEndpoints(const FutureTaskRun& run) {
  absl::erase_if(completion_endpoints_, [&run](const auto& entry) {
    auto& state = *entry.second;
    if (state.run.get() != &run) {
      return false;
    }
    FutureCompletionEndpoint::Close(state, FutureCompletionEndpoint::CloseReason::Shutdown);
    return true;
  });
}

std::expected<void, FutureCompletionError> FutureCompletionEndpoint::Complete(Value value) const {
  return Settle({.state = FutureState::Succeeded, .value = value, .diagnostic = {}});
}

std::expected<void, FutureCompletionError> FutureCompletionEndpoint::CompleteIn(Heap& heap,
                                                                                Value value) const {
  return Settle({.state = FutureState::Succeeded, .value = value, .diagnostic = {}}, &heap);
}

std::expected<void, FutureCompletionError> FutureCompletionEndpoint::CompleteText(
    std::string text) const {
  if (lease_ == nullptr) {
    return std::unexpected(FutureCompletionError::Closed);
  }
  auto state = lease_->state;
  const std::scoped_lock lock(state->mutex);
  if (auto checked = state->CheckRequest(); !checked) {
    return checked;
  }
  auto* scheduler = state->scheduler;
  state->completed = true;
  scheduler->PostInRun(
      [state, text = std::move(text)] {
        const std::scoped_lock completion_lock(state->mutex);
        if (state->scheduler == nullptr) {
          return;
        }
        auto& completer = *state->completer;
        auto status = ExecutionStatus::TypeError;
        if (!InvalidUtf8Offset(text)) {
          auto* string = completer.future().heap().AllocateString(text);
          if (string != nullptr) {
            std::ignore = completer.Complete(Value::String(string));
            state->Retire();
            return;
          }
          status = ExecutionStatus::OutOfMemory;
        }
        std::ignore = completer.CompleteError(
            Value::Null(), {.message = {}, .stack_trace = {}, .status = status, .origin = {}});
        state->Retire();
      },
      state->run);
  return {};
}

std::expected<void, FutureCompletionError> FutureCompletionEndpoint::CompleteError(
    Value error) const {
  return CompleteError(error, {});
}

std::expected<void, FutureCompletionError> FutureCompletionEndpoint::CompleteError(
    Value error,
    FutureDiagnostic diagnostic) const {
  return Settle(
      {.state = FutureState::Failed, .value = error, .diagnostic = std::move(diagnostic)});
}

std::expected<void, FutureCompletionError> FutureCompletionEndpoint::Settle(
    FutureResult result,
    Heap* expected_heap) const {
  if (lease_ == nullptr) {
    return std::unexpected(FutureCompletionError::Closed);
  }
  auto& state = *lease_->state;
  const std::scoped_lock lock(state.mutex);
  if (auto checked = state.CheckRequest(); !checked) {
    return checked;
  }
  auto& completer = *state.completer;
  if (expected_heap != nullptr && &completer.future().heap() != expected_heap) {
    return std::unexpected(FutureCompletionError::DifferentIsolate);
  }
  const auto completion = completer.Settle(std::move(result));
  if (completion) {
    state.completed = true;
    state.Retire();
  }
  return completion;
}

}  // namespace vela::runtime
