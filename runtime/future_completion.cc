#include "runtime/future.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <utility>

#include "runtime/value.h"

namespace vela::runtime {

struct FutureCompletionEndpoint::State final {
  State(FutureScheduler& owner, std::uint64_t completion_identity, Completer target)
      : scheduler(&owner),
        identity(completion_identity),
        completer(std::make_unique<Completer>(std::move(target))) {}

  void Retire() const {
    auto* owner = scheduler;
    const auto completion_identity = identity;
    owner->PostMicrotask(
        [owner, completion_identity] { owner->RetireCompletionEndpoint(completion_identity); });
  }

  std::mutex mutex;
  FutureScheduler* scheduler;
  std::uint64_t identity;
  std::unique_ptr<Completer> completer;
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

void FutureCompletionEndpoint::Close(State& state) {
  std::unique_ptr<Completer> retired;
  {
    const std::scoped_lock lock(state.mutex);
    retired = std::move(state.completer);
    state.scheduler = nullptr;
  }
}

std::expected<void, FutureCompletionError> FutureCompletionEndpoint::Complete(Value value) const {
  return Settle({.state = FutureState::Succeeded, .value = value, .diagnostic = {}});
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
    FutureResult result) const {
  if (lease_ == nullptr) {
    return std::unexpected(FutureCompletionError::Closed);
  }
  auto& state = *lease_->state;
  const std::scoped_lock lock(state.mutex);
  if (state.completed) {
    return std::unexpected(FutureCompletionError::AlreadyCompleted);
  }
  if (state.scheduler == nullptr) {
    return std::unexpected(FutureCompletionError::Closed);
  }
  auto& completer = *state.completer;
  const auto completion = result.state == FutureState::Succeeded
                              ? completer.Complete(result.value)
                              : completer.CompleteError(result.value, std::move(result.diagnostic));
  if (completion) {
    state.completed = true;
    state.Retire();
  }
  return completion;
}

}  // namespace vela::runtime
