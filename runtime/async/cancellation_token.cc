#include "runtime/async/future.h"

#include <atomic>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>

namespace vela::runtime {

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

}  // namespace vela::runtime
