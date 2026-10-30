#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/functional/any_invocable.h>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "runtime/execution_context.h"
#include "runtime/external_resources.h"
#include "runtime/value.h"

namespace vela::runtime {

class FutureScheduler;
class Heap;
class RootRange;
class FutureCompletionEndpoint;

enum class FutureState : std::uint8_t { Pending, Succeeded, Failed };
enum class FutureCompletionError : std::uint8_t {
  AlreadyCompleted,
  SelfAdoption,
  DifferentIsolate,
  Closed,
};
enum class FutureBindingError : std::uint8_t { DifferentIsolate };
[[nodiscard]] ExecutionStatus FutureCompletionStatus(FutureCompletionError error) noexcept;
enum class FuturePumpMode : std::uint8_t { AvailableWork, WaitForHost, ReadyWork };

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
  FunctionIdentity identity;
};

struct FutureDiagnostic final {
  std::string message;
  std::vector<FutureTraceFrame> stack_trace;
  ExecutionStatus status = ExecutionStatus::Thrown;
  ErrorOrigin origin;
};

struct FutureResult final {
  FutureState state = FutureState::Pending;
  Value value;
  FutureDiagnostic diagnostic;
};

// Future 的状态只允许由所属 Isolate 线程推进；Complete 可由宿主线程安全投递。
class Future final {
  struct SharedState;
  struct Observer;

 public:
  using Callback = std::function<void(FutureState, ::vela::runtime::Value)>;

  explicit Future(FutureScheduler* scheduler = nullptr);
  ~Future();
  Future(const Future& other);
  Future& operator=(const Future& other);
  Future(Future&& other) noexcept;
  Future& operator=(Future&& other) noexcept;
  // 堆对象与挂起回调使用内部句柄；它们的值由可达的 Future 对象精确追踪。
  [[nodiscard]] Future Internal() const;
  [[nodiscard]] Future Rooted() const;
  // 运行时队列和完成端点的临时强根；参与 GC，但不要求宿主在 Reload 前手动释放。
  [[nodiscard]] Future Retained() const;
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
  [[nodiscard]] static Future Sleep(std::chrono::milliseconds delay, FutureScheduler& scheduler);
  [[nodiscard]] FutureState state() const noexcept;
  [[nodiscard]] std::optional<ExecutionContext> creation_context() const noexcept;
  // 借用当前完成记录；所属线程使用，Future 的句柄覆盖借用期。
  [[nodiscard]] const FutureResult& result() const noexcept;
  [[nodiscard]] bool is_pending() const noexcept { return state() == FutureState::Pending; }
  [[nodiscard]] bool IsCompleted() const noexcept;
  [[nodiscard]] std::optional<::vela::runtime::Value> value() const;
  [[nodiscard]] std::optional<::vela::runtime::Value> error() const;
  [[nodiscard]] std::optional<FutureDiagnostic> error_diagnostic() const;
  void Observe(Callback callback) const;
  void ObserveResult(std::function<void(const FutureResult&)> callback,
                     std::shared_ptr<std::vector<::vela::runtime::Value>> references = {}) const;
  std::expected<void, FutureCompletionError> Adopt(const Future& source);
  // 所属线程同步检查 adoption；跨线程返回投递是否成功，adoption 失败发布为 Future 错误。
  // 跨线程传入的堆值必须属于该 Isolate，且在所属线程接收前持续保根。
  std::expected<void, FutureCompletionError> Complete(::vela::runtime::Value value);
  std::expected<void, FutureCompletionError> CompleteError(::vela::runtime::Value error);
  std::expected<void, FutureCompletionError> CompleteError(::vela::runtime::Value error,
                                                           FutureDiagnostic diagnostic);
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
  enum class Rooting : std::uint8_t { External, Traced, Retained };
  void Retain() noexcept;
  void Unroot() noexcept;
  void ReportUnhandledError() const;
  void ReportHandledError() const;
  explicit Future(std::shared_ptr<SharedState> state, Rooting rooting);
  std::expected<void, FutureCompletionError> Settle(FutureResult result);
  void Publish(FutureResult result);
  void NotifyObserver(std::function<void(const FutureResult&)> callback,
                      std::shared_ptr<std::vector<::vela::runtime::Value>> references,
                      std::optional<ExecutionContext> context) const;
  [[nodiscard]] std::expected<void, FutureCompletionError> CheckAdoption(
      const Future& source) const;
  void Follow(const Future& source);
  void Release() noexcept;
  friend class Completer;
  friend class FutureScheduler;
  std::shared_ptr<SharedState> state_;
  Rooting rooting_ = Rooting::External;
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
  // 先取出供脚本返回的 Future，再把完成能力转交给可跨运行停止边界存活的端点。
  [[nodiscard]] FutureCompletionEndpoint Detach() &&;

 private:
  Future future_;
  std::shared_ptr<HostEventSource> producer_;
};

// 端点关闭后不再持有 Future、堆或调度器；复制的宿主句柄可以晚于 Isolate 销毁。
// 完成与关闭串行化。跨线程堆值仍须由宿主按原有契约保根，不能跨运行迁移。
class FutureCompletionEndpoint final {
 public:
  FutureCompletionEndpoint() = default;
  [[nodiscard]] std::expected<void, FutureCompletionError> Complete(Value value) const;
  [[nodiscard]] std::expected<void, FutureCompletionError> CompleteError(Value error) const;
  [[nodiscard]] std::expected<void, FutureCompletionError> CompleteError(
      Value error,
      FutureDiagnostic diagnostic) const;

 private:
  friend class Completer;
  friend class FutureScheduler;
  struct State;
  struct Lease;
  explicit FutureCompletionEndpoint(std::shared_ptr<Lease> lease);
  [[nodiscard]] static FutureCompletionEndpoint Create(FutureScheduler& scheduler,
                                                       Completer completer);
  static void Close(State& state);
  [[nodiscard]] std::expected<void, FutureCompletionError> Settle(FutureResult result) const;
  std::shared_ptr<Lease> lease_;
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

 private:
  friend class FutureScheduler;
  struct State final {
    std::mutex mutex;
    std::atomic_bool cancelled{false};
    std::function<void()> callback;
    FutureScheduler* scheduler = nullptr;
    std::optional<ExecutionContext> context;
  };
  static void Close(State& state) noexcept;
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

  void PostMicrotask(absl::AnyInvocable<void()> task);
  [[nodiscard]] std::optional<ExecutionContext> context() const noexcept { return context_; }
  void SetExecutionContext(std::optional<ExecutionContext> context) noexcept { context_ = context; }
  [[nodiscard]] std::uint64_t NewErrorIdentity() noexcept {
    return next_error_identity_.fetch_add(1, std::memory_order_relaxed);
  }
  void SetUnhandledErrorHandler(ErrorCallback callback);
  void SetHandledErrorHandler(ErrorCallback callback);
  [[nodiscard]] Completer CreateCompleter();
  [[nodiscard]] Completer CreateHostCompleter();
  [[nodiscard]] Timer After(std::chrono::milliseconds delay, std::function<void()> callback);
  [[nodiscard]] Timer Periodic(std::chrono::milliseconds interval, std::function<void()> callback);
  // 执行一个宿主事件及其产生的全部微任务；返回 false 表示当前没有可推进的工作。
  [[nodiscard]] bool PumpOne(FuturePumpMode mode = FuturePumpMode::AvailableWork);
  [[nodiscard]] bool PumpUntil(Future& root, FuturePumpMode mode = FuturePumpMode::AvailableWork);
  [[nodiscard]] bool IsOwnerThread() const noexcept;
  [[nodiscard]] static bool CanSchedule(std::chrono::milliseconds duration) noexcept;
  void AddSuspendedFrame(std::size_t saved_bytes) noexcept;
  void RemoveSuspendedFrame(std::size_t saved_bytes) noexcept;
  [[nodiscard]] std::size_t suspended_frame_count() const noexcept { return suspended_frames_; }
  [[nodiscard]] std::size_t saved_state_bytes() const noexcept { return saved_state_bytes_; }
  [[nodiscard]] ExternalResources& external_resources() noexcept { return external_resources_; }
  void AppendExternalResources(std::vector<ExternalResourceInfo>& result) const;

 private:
  friend class Future;
  friend class HostEventSource;
  friend class FutureCompletionEndpoint;
  ExternalResources external_resources_;
  std::atomic_size_t host_accesses_{0};
  void RetireCompletionEndpoint(std::uint64_t identity);
  std::uint64_t next_completion_identity_ = 0;
  absl::flat_hash_map<std::uint64_t, std::shared_ptr<FutureCompletionEndpoint::State>>
      completion_endpoints_;
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
    absl::AnyInvocable<void()> action;
  };
  static_assert(std::atomic<Message*>::is_always_lock_free);
  void ReceiveMessages();
  std::atomic<Message*> incoming_{nullptr};
  std::deque<absl::AnyInvocable<void()>> host_events_;
  std::counting_semaphore<> wakeup_{0};
  std::atomic_size_t host_sources_{0};
  std::size_t suspended_frames_ = 0;
  std::size_t saved_state_bytes_ = 0;
  struct Microtask final {
    std::optional<ExecutionContext> context;
    absl::AnyInvocable<void()> action;
  };
  std::deque<Microtask> microtasks_;
  std::deque<absl::AnyInvocable<void()>> checkpoints_;
  std::priority_queue<Event, std::vector<Event>, EventOrder> events_;
  std::uint64_t next_sequence_ = 0;
  ErrorCallback unhandled_error_handler_;
  ErrorCallback handled_error_handler_;
  const std::thread::id owner_ = std::this_thread::get_id();
  std::optional<ExecutionContext> context_;
  std::atomic_uint64_t next_error_identity_{1};
};

}  // namespace vela::runtime
