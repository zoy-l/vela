#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <semaphore>

#include <absl/functional/any_invocable.h>

#include "base/execution_status.h"
#include "runtime/native/native_stack.h"

namespace vela::runtime {

class NativeExecution;
class NativeDebugger;
enum class NativeSafepointError : std::uint8_t { AlreadyPending, Stopped };

// 请求地址随队列固定；借用深度只由 Isolate 线程读写，生成代码与运行时共用这一状态。
struct NativeControlState final {
  static constexpr auto kUnknownStackLimit = std::numeric_limits<std::uintptr_t>::max();

  const std::atomic<std::uint8_t>* requests = nullptr;
  std::uint64_t host_borrows = 0;
  NativeExecution* host_execution = nullptr;
  std::uint64_t initializations = 0;
  std::uintptr_t callback_stack_limit = kUnknownStackLimit;
};

inline constexpr std::size_t kNativeControlCacheLineBytes = 64;

// 请求字和热控制字段共处同一缓存行，避免依赖宿主栈帧的物理布局。
struct alignas(kNativeControlCacheLineBytes) NativeSafepointQueue final {
  NativeControlState control;
  // 各请求来源独占一个位；慢路径用 acquire 消费已发布的数据。
  std::atomic<std::uint8_t> requests{0};
  std::mutex mutex;
  std::deque<absl::AnyInvocable<void()>> actions;
  bool closed = false;
  std::weak_ptr<std::counting_semaphore<>> wakeup;
};

[[nodiscard]] inline std::expected<std::uintptr_t, ExecutionStatus> CurrentNativeStackLimit(
    [[maybe_unused]] const NativeControlState& control) noexcept {
#ifndef _WIN32
  // POSIX Isolate 固定在创建线程，其原生栈边界在 Isolate 生命周期内不变。
  const auto cached_limit = control.callback_stack_limit;
  if (cached_limit != NativeControlState::kUnknownStackLimit) [[likely]] {
    return cached_limit;
  }
#endif
  return CurrentNativeStackLimit();
}

// 跨线程只拥有队列控制状态；关闭后投递不会访问已退休的 Isolate。
class NativeSafepointEndpoint final {
 public:
  void RequestStop() const noexcept;
  [[nodiscard]] std::expected<void, NativeSafepointError> Post(
      absl::AnyInvocable<void()> action) const;

 private:
  friend class NativeControl;
  explicit NativeSafepointEndpoint(std::shared_ptr<NativeSafepointQueue> queue);
  std::shared_ptr<NativeSafepointQueue> queue_;
};

// 停止请求可来自任意线程；宿主借用计数及安全点检查只在 Isolate 所属线程操作。
// 重入脚本仍处于外层宿主借用内，必须等最外层 binding 正常返回后才能停止旧运行。
class NativeControl final {
 public:
  explicit NativeControl(std::weak_ptr<std::counting_semaphore<>> wakeup = {});
  ~NativeControl();
  NativeControl(const NativeControl&) = delete;
  NativeControl& operator=(const NativeControl&) = delete;
  NativeControl(NativeControl&&) = delete;
  NativeControl& operator=(NativeControl&&) = delete;
  [[nodiscard]] NativeSafepointEndpoint endpoint() const;
  [[nodiscard]] NativeControlState& state() noexcept { return queue_->control; }
  void SetDebugger(NativeDebugger* debugger) noexcept;
  void RequestStop() const noexcept;
  void EnterInitialization() noexcept;
  void ExitInitialization() noexcept;
  [[nodiscard]] ExecutionStatus Poll() const noexcept;
  [[nodiscard]] bool HasHostBorrow() const noexcept;
  [[nodiscard]] bool IsStopRequested() const noexcept;
  [[nodiscard]] bool HasReadySafepoint() const noexcept;
  [[nodiscard]] bool CanPublish() const noexcept;
  [[nodiscard]] bool IsInitializing() const noexcept;
  [[nodiscard]] bool IsProcessingSafepoint() const noexcept { return processing_; }
  // 所属线程提交的一次性动作；宿主重入与初始化结束后在精确保根的安全点执行。
  [[nodiscard]] std::expected<void, NativeSafepointError> ScheduleSafepoint(
      absl::AnyInvocable<void()> action);
  void CancelSafepoint() noexcept;
  [[nodiscard]] ExecutionStatus ProcessSafepoint(NativeExecution* execution = nullptr);

 private:
  [[nodiscard]] bool CanProcessSafepoint() const noexcept;
  std::shared_ptr<NativeSafepointQueue> queue_;
  absl::AnyInvocable<void()> pending_;
  bool processing_ = false;
  NativeDebugger* debugger_ = nullptr;
  bool debugging_ = false;
};

// 初始化可以调用脚本或宿主；该作用域覆盖成功、异常及停止返回，禁止跨版本提交半成品。
class NativeInitializationScope final {
 public:
  explicit NativeInitializationScope(NativeControl& control) noexcept;
  ~NativeInitializationScope();
  NativeInitializationScope(const NativeInitializationScope&) = delete;
  NativeInitializationScope& operator=(const NativeInitializationScope&) = delete;
  NativeInitializationScope(NativeInitializationScope&&) = delete;
  NativeInitializationScope& operator=(NativeInitializationScope&&) = delete;

 private:
  NativeControl* control_;
};

extern "C" std::uint8_t vela_native_v3_safepoint(NativeExecution* execution) noexcept;

}  // namespace vela::runtime
