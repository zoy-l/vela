#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "base/execution_status.h"

namespace vela::runtime {

class NativeExecution;

// 停止请求可来自任意线程；宿主借用计数及安全点检查只在 Isolate 所属线程操作。
// 重入脚本仍处于外层宿主借用内，必须等最外层 binding 正常返回后才能停止旧运行。
class NativeControl final {
 public:
  void RequestStop() noexcept;
  void EnterHost() noexcept;
  void ExitHost() noexcept;
  void EnterInitialization() noexcept;
  void ExitInitialization() noexcept;
  [[nodiscard]] ExecutionStatus Poll() const noexcept;
  [[nodiscard]] bool HasHostBorrow() const noexcept;
  [[nodiscard]] bool IsStopRequested() const noexcept;
  [[nodiscard]] bool CanPublish() const noexcept;

 private:
  std::atomic_bool stop_requested_{false};
  std::size_t host_borrows_ = 0;
  std::size_t initializations_ = 0;
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

}  // namespace vela::runtime
