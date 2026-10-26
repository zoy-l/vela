#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <semaphore>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/execution_status.h"
#include "runtime/value.h"

namespace vela::runtime {

class FutureScheduler;
class Heap;
class RootRange;

enum class FutureState : std::uint8_t { Pending, Succeeded, Failed };
enum class FutureCompletionError : std::uint8_t {
  AlreadyCompleted,
  SelfAdoption,
  DifferentIsolate,
};
enum class FutureBindingError : std::uint8_t { DifferentIsolate };
[[nodiscard]] ExecutionStatus FutureCompletionStatus(FutureCompletionError error) noexcept;
enum class FutureWaitMode : std::uint8_t { WaitAll, EagerError };
enum class FuturePumpMode : std::uint8_t { AvailableWork, WaitForHost };

class HostEventSource final {
 public:
  explicit HostEventSource(FutureScheduler& scheduler);
  ~HostEventSource();
  HostEventSource(const HostEventSource&) = delete;
  HostEventSource& operator=(const HostEventSource&) = delete;
  HostEventSource(HostEventSource&&) = delete;
  HostEventSource& operator=(HostEventSource&&) = delete;
  void Release() noexcept;

 private:
  FutureScheduler* scheduler_;
  std::atomic_bool live_{true};
};

struct FutureTraceFrame final {
  std::string function;
  std::string source_path;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::size_t instruction = 0;
};

struct FutureDiagnostic final {
  std::string message;
  std::vector<FutureTraceFrame> stack_trace;
  ExecutionStatus status = ExecutionStatus::Thrown;
};

struct FutureResult final {
  FutureState state = FutureState::Pending;
  Value value;
  FutureDiagnostic diagnostic;
};

// Future 的状态只允许由所属 Isolate 线程推进；Complete 可由宿主线程安全投递。
class Future final {
  struct SharedState;

 public:
  using Callback = std::function<void(FutureState, ::vela::runtime::Value)>;
  using Continuation = std::function<Future(::vela::runtime::Value)>;

  explicit Future(FutureScheduler* scheduler = nullptr);
  ~Future();
  Future(const Future& other);
  Future& operator=(const Future& other);
  Future(Future&& other) noexcept;
  Future& operator=(Future&& other) noexcept;
  // 堆对象与挂起回调使用内部句柄；它们的值由可达的 Future 对象精确追踪。
  [[nodiscard]] Future Internal() const;
  [[nodiscard]] Future Rooted() const;
  void MakeHeapOwned() noexcept;
  void KeepAlive(std::shared_ptr<std::vector<::vela::runtime::Value>> references);
  class Root final {
   public:
    void Trace(Heap& heap) const noexcept;
    [[nodiscard]] bool IsExpired() const noexcept { return state_.expired(); }

   private:
    friend class Future;
    explicit Root(const std::shared_ptr<SharedState>& state) : state_(state) {}
    std::weak_ptr<SharedState> state_;
  };
  [[nodiscard]] static Future Delayed(std::chrono::milliseconds delay,
                                      FutureScheduler& scheduler,
                                      std::function<Future()> computation = {});
  [[nodiscard]] static Future Sleep(std::chrono::milliseconds delay, FutureScheduler& scheduler);
  [[nodiscard]] static Future Wait(std::span<const Future> values,
                                   FutureScheduler& scheduler,
                                   FutureWaitMode mode = FutureWaitMode::WaitAll);
  [[nodiscard]] static Future Race(std::span<const Future> values, FutureScheduler& scheduler);
  [[nodiscard]] FutureState state() const noexcept;
  [[nodiscard]] bool is_pending() const noexcept { return state() == FutureState::Pending; }
  [[nodiscard]] bool IsCompleted() const noexcept;
  [[nodiscard]] std::optional<::vela::runtime::Value> value() const;
  [[nodiscard]] std::optional<::vela::runtime::Value> error() const;
  [[nodiscard]] std::optional<FutureDiagnostic> error_diagnostic() const;
  void Observe(Callback callback) const;
  void ObserveResult(std::function<void(const FutureResult&)> callback,
                     std::shared_ptr<std::vector<::vela::runtime::Value>> references = {}) const;
  std::expected<void, FutureCompletionError> Adopt(const Future& source);
  std::expected<void, FutureCompletionError> Complete(::vela::runtime::Value value);
  std::expected<void, FutureCompletionError> CompleteError(::vela::runtime::Value error);
  std::expected<void, FutureCompletionError> CompleteError(::vela::runtime::Value error,
                                                           FutureDiagnostic diagnostic);
  [[nodiscard]] Future Then(Continuation on_value, Continuation on_error = {}) const;
  [[nodiscard]] Future CatchError(Continuation on_error) const;
  [[nodiscard]] Future WhenComplete(const std::function<void()>& action) const;
  [[nodiscard]] Future Timeout(std::chrono::milliseconds duration,
                               FutureScheduler& scheduler,
                               Continuation on_timeout = {}) const;
  [[nodiscard]] static Future Value(::vela::runtime::Value value,
                                    FutureScheduler* scheduler = nullptr);
  [[nodiscard]] static Future Error(::vela::runtime::Value error,
                                    FutureScheduler* scheduler = nullptr);
  // GC 在扫描 Future 堆对象时追踪结果、监听器与活动任务引用。
  void Trace(Heap& heap) const noexcept;
  void AttachHeap(Heap& heap);
  [[nodiscard]] std::expected<void, FutureBindingError> Bind(FutureScheduler& scheduler,
                                                             Heap& heap);

 private:
  enum class Rooting : std::uint8_t { External, Traced };
  void ReportUnhandledError() const;
  void ReportHandledError() const;
  explicit Future(std::shared_ptr<SharedState> state, Rooting rooting);
  std::expected<void, FutureCompletionError> Settle(FutureResult result);
  void Publish(const FutureResult& result);
  [[nodiscard]] bool CanAdopt(const Future& source) const;
  void Follow(const Future& source);
  void AdoptComputation(const Future& source);
  void Release() noexcept;
  friend class Completer;
  friend class FutureScheduler;
  std::shared_ptr<SharedState> state_;
  bool external_ = true;
};

class Completer final {
 public:
  Completer() = default;
  explicit Completer(Future future) : future_(std::move(future)) {}
  Completer(Future future, std::shared_ptr<HostEventSource> producer)
      : future_(std::move(future)), producer_(std::move(producer)) {}
  [[nodiscard]] const Future& future() const noexcept { return future_; }
  std::expected<void, FutureCompletionError> Complete(::vela::runtime::Value value);
  std::expected<void, FutureCompletionError> CompleteError(::vela::runtime::Value error);
  std::expected<void, FutureCompletionError> CompleteError(::vela::runtime::Value error,
                                                           FutureDiagnostic diagnostic);

 private:
  Future future_;
  std::shared_ptr<HostEventSource> producer_;
};

class CancellationToken final {
 public:
  CancellationToken();
  void Cancel() noexcept;
  [[nodiscard]] bool IsCancelled() const noexcept;
  void Observe(std::function<void()> callback);

 private:
  struct State;
  std::shared_ptr<State> state_;
};

class Timer final {
 public:
  Timer() = default;
  void Cancel() noexcept;
  [[nodiscard]] bool IsCancelled() const noexcept;

 private:
  friend class FutureScheduler;
  struct State final {
    std::atomic_bool cancelled{false};
    std::function<void()> callback;
    FutureScheduler* scheduler = nullptr;
  };
  explicit Timer(std::shared_ptr<State> state) : state_(std::move(state)) {}
  std::shared_ptr<State> state_ = std::make_shared<State>();
};

class FutureScheduler final {
 public:
  using ErrorCallback = std::function<void(const Future&, ::vela::runtime::Value)>;

  using Clock = std::chrono::steady_clock;
  FutureScheduler() = default;
  ~FutureScheduler();
  void Shutdown();
  FutureScheduler(const FutureScheduler&) = delete;
  FutureScheduler& operator=(const FutureScheduler&) = delete;
  FutureScheduler(FutureScheduler&&) = delete;
  FutureScheduler& operator=(FutureScheduler&&) = delete;

  void PostMicrotask(std::function<void()> task);
  void SetUnhandledErrorHandler(ErrorCallback callback);
  void SetHandledErrorHandler(ErrorCallback callback);
  [[nodiscard]] Completer CreateCompleter();
  [[nodiscard]] Completer CreateHostCompleter();
  [[nodiscard]] Timer After(std::chrono::milliseconds delay, std::function<void()> callback);
  [[nodiscard]] Timer Periodic(std::chrono::milliseconds interval, std::function<void()> callback);
  // 执行一个宿主事件及其产生的全部微任务；返回 false 表示当前没有可推进的工作。
  [[nodiscard]] bool PumpOne(FuturePumpMode mode = FuturePumpMode::AvailableWork);
  [[nodiscard]] bool PumpUntil(Future& root, FuturePumpMode mode = FuturePumpMode::AvailableWork);
  [[nodiscard]] std::size_t pending_microtasks() const noexcept;
  [[nodiscard]] bool IsOwnerThread() const noexcept;
  [[nodiscard]] static bool CanSchedule(std::chrono::milliseconds duration) noexcept;
  void AddSuspendedFrame(std::size_t saved_bytes) noexcept;
  void RemoveSuspendedFrame(std::size_t saved_bytes) noexcept;
  [[nodiscard]] std::size_t suspended_frame_count() const noexcept { return suspended_frames_; }
  [[nodiscard]] std::size_t saved_state_bytes() const noexcept { return saved_state_bytes_; }

 private:
  friend class Future;
  friend class HostEventSource;
  struct Event final {
    Clock::time_point deadline;
    std::uint64_t sequence = 0;
    std::chrono::milliseconds interval{0};
    std::shared_ptr<Timer::State> state;
  };
  struct EventOrder final {
    bool operator()(const Event& left, const Event& right) const noexcept {
      return left.deadline != right.deadline ? left.deadline > right.deadline
                                             : left.sequence > right.sequence;
    }
  };

  void DrainMicrotasks();
  void Fire(Event event);
  void NotifyUnhandledError(const Future& future, ::vela::runtime::Value error);
  void NotifyHandledError(const Future& future, ::vela::runtime::Value error);
  // 生产者只发布不可变消息；消费者独占本地队列与 Future/GC 状态。
  struct Message final {
    Message* next = nullptr;
    std::function<void()> action;
  };
  static_assert(std::atomic<Message*>::is_always_lock_free);
  void ReceiveMessages();
  std::atomic<Message*> incoming_{nullptr};
  std::deque<std::function<void()>> host_events_;
  std::counting_semaphore<> wakeup_{0};
  std::atomic_size_t host_sources_{0};
  std::size_t suspended_frames_ = 0;
  std::size_t saved_state_bytes_ = 0;
  std::deque<std::function<void()>> microtasks_;
  std::deque<std::function<void()>> checkpoints_;
  std::priority_queue<Event, std::vector<Event>, EventOrder> events_;
  std::uint64_t next_sequence_ = 0;
  ErrorCallback unhandled_error_handler_;
  ErrorCallback handled_error_handler_;
  const std::thread::id owner_ = std::this_thread::get_id();
};

}  // namespace vela::runtime
