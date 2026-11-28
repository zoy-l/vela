#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

#include "benchmarks/production/allocation.h"

namespace vela::benchmarks::production {

// 独立的单线程执行器：此文件不引用 Vela Future、Isolate 或调度器。
template <bool Counted>
class Scheduler final {
 public:
  using Clock = std::chrono::steady_clock;
  using Handle = std::coroutine_handle<>;
  void Post(Handle handle) { ready_.push_back(handle); }
  struct Yield final {
    Scheduler* scheduler = nullptr;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(Handle handle) const { scheduler->Post(handle); }
    void await_resume() const noexcept {}
  };
  struct Delay final {
    Scheduler* scheduler = nullptr;
    Clock::duration duration{};
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(Handle handle) const {
      scheduler->timers_.push({Clock::now() + duration, scheduler->sequence_++, handle});
    }
    void await_resume() const noexcept {}
  };
  void Drain() {
    while (!ready_.empty() || !timers_.empty()) {
      if (!ready_.empty()) {
        const auto handle = ready_.front();
        ready_.pop_front();
        handle.resume();
      } else {
        const auto timer = timers_.top();
        timers_.pop();
        if (timer.deadline > Clock::now()) {
          std::this_thread::sleep_until(timer.deadline);
        }
        timer.handle.resume();
      }
    }
  }

 private:
  struct Timer final {
    Clock::time_point deadline;
    std::uint64_t sequence{};
    Handle handle;
    bool operator>(const Timer& other) const noexcept {
      return deadline > other.deadline || (deadline == other.deadline && sequence > other.sequence);
    }
  };
  std::deque<Handle, Allocator<Handle, Counted>> ready_;
  std::priority_queue<Timer, std::vector<Timer, Allocator<Timer, Counted>>, std::greater<>> timers_;
  std::uint64_t sequence_ = 0;
};

template <bool Counted>
class Task final {
 public:
  struct promise_type;
  using Handle = std::coroutine_handle<promise_type>;
  struct promise_type final {
    template <class... Args>
    explicit promise_type(Scheduler<Counted>* owner,
                          [[maybe_unused]] const Args&... args /*unused*/)
        : scheduler(owner) {}
    static void* operator new(std::size_t size) {
      if constexpr (Counted) {
        return AllocateCounted(ActiveAllocationLedger(), size);
      }
      return ::operator new(size);
    }
    static void operator delete(void* pointer, [[maybe_unused]] std::size_t size) noexcept {
      if constexpr (Counted) {
        FreeCounted(ActiveAllocationLedger(), pointer);
      } else {
        ::operator delete(pointer);
      }
    }
    Task get_return_object() noexcept { return Task(Handle::from_promise(*this)); }
    [[nodiscard]] std::suspend_never initial_suspend() const noexcept { return {}; }
    struct Final final {
      [[nodiscard]] bool await_ready() const noexcept { return false; }
      void await_suspend(Handle handle) const noexcept {
        auto& promise = handle.promise();
        if (promise.continuation) {
          promise.scheduler->Post(promise.continuation);
        }
      }
      void await_resume() const noexcept {}
    };
    [[nodiscard]] Final final_suspend() const noexcept { return {}; }
    void return_value(std::int64_t result) noexcept { value = result; }
    void unhandled_exception() const noexcept { std::terminate(); }
    Scheduler<Counted>* scheduler;
    std::coroutine_handle<> continuation;
    std::int64_t value = 0;
  };
  explicit Task(Handle handle) noexcept : handle_(handle) {}
  ~Task() {
    if (handle_) {
      handle_.destroy();
    }
  }
  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  Task& operator=(Task&&) = delete;
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  [[nodiscard]] bool await_ready() const noexcept { return handle_.done(); }
  void await_suspend(std::coroutine_handle<> continuation) noexcept {
    handle_.promise().continuation = continuation;
  }
  [[nodiscard]] std::int64_t await_resume() const noexcept { return handle_.promise().value; }
  [[nodiscard]] bool IsDone() const noexcept { return handle_.done(); }
  [[nodiscard]] std::int64_t value() const noexcept { return handle_.promise().value; }

 private:
  Handle handle_;
};

template <bool Counted>
struct Event final {
  bool ready = false;
  std::vector<std::coroutine_handle<>, Allocator<std::coroutine_handle<>, Counted>> waiters;
  [[nodiscard]] bool await_ready() const noexcept { return ready; }
  void await_suspend(std::coroutine_handle<> handle) { waiters.push_back(handle); }
  void await_resume() const noexcept {}
  void Signal(Scheduler<Counted>* scheduler) {
    ready = true;
    for (const auto handle : waiters) {
      scheduler->Post(handle);
    }
    waiters.clear();
  }
};

}  // namespace vela::benchmarks::production
