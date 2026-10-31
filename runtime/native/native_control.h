#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>

#include <absl/functional/any_invocable.h>

#include "base/execution_status.h"

namespace vela::runtime {

class NativeExecution;
class NativeDebugger;
enum class NativeSafepointError : std::uint8_t { AlreadyPending, Stopped };

struct NativeSafepointQueue final {
  std::mutex mutex;
  std::deque<absl::AnyInvocable<void()>> actions;
  std::atomic_bool available{false};
  std::atomic_bool stop_requested{false};
  bool closed = false;
};

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
  NativeControl();
  ~NativeControl();
  NativeControl(const NativeControl&) = delete;
  NativeControl& operator=(const NativeControl&) = delete;
  NativeControl(NativeControl&&) = delete;
  NativeControl& operator=(NativeControl&&) = delete;
  [[nodiscard]] NativeSafepointEndpoint endpoint() const;
  void SetDebugger(NativeDebugger* debugger) noexcept;
  void RequestStop() const noexcept;
  void EnterHost() noexcept;
  void ExitHost() noexcept;
  void EnterInitialization() noexcept;
  void ExitInitialization() noexcept;
  void ExitInitializations(std::uint32_t count) noexcept;
  [[nodiscard]] ExecutionStatus Poll() const noexcept;
  [[nodiscard]] bool HasHostBorrow() const noexcept;
  [[nodiscard]] bool IsStopRequested() const noexcept;
  [[nodiscard]] bool CanPublish() const noexcept;
  [[nodiscard]] bool IsInitializing() const noexcept;
  [[nodiscard]] bool IsProcessingSafepoint() const noexcept { return processing_; }
  // 所属线程提交的一次性动作；宿主重入与初始化结束后在精确保根的安全点执行。
  [[nodiscard]] std::expected<void, NativeSafepointError> ScheduleSafepoint(
      std::function<void()> action);
  void CancelSafepoint() noexcept;
  [[nodiscard]] ExecutionStatus ProcessSafepoint(NativeExecution* execution = nullptr);

 private:
  std::size_t host_borrows_ = 0;
  std::size_t initializations_ = 0;
  std::function<void()> pending_;
  bool processing_ = false;
  NativeDebugger* debugger_ = nullptr;
  bool debugging_ = false;
  std::shared_ptr<NativeSafepointQueue> queue_;
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
extern "C" void vela_native_v3_host_enter(NativeExecution* execution) noexcept;
extern "C" void vela_native_v3_host_exit(NativeExecution* execution) noexcept;
extern "C" void vela_native_v3_initialization_enter(NativeExecution* execution) noexcept;
extern "C" void vela_native_v3_initialization_exit(NativeExecution* execution,
                                                   std::uint32_t count) noexcept;

}  // namespace vela::runtime
