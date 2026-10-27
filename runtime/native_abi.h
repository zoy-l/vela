#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;

inline constexpr std::uint32_t kNativeAbiVersion = 1;
inline constexpr std::size_t kNativeValueBytes = 16;
inline constexpr std::size_t kNativeValueReservedBytes = 7;

enum class NativeValueKind : std::uint8_t {
  Null,
  Boolean,
  Integer,
  Double,
  Object,
  String,
};

// 仅跨宿主包装边界使用；编译单元内部的参数和局部值保持类型化。
struct NativeValue final {
  std::uint64_t payload = 0;
  NativeValueKind kind = NativeValueKind::Null;
  std::array<std::uint8_t, kNativeValueReservedBytes> reserved = {};
};
static_assert(sizeof(NativeValue) == kNativeValueBytes);

class NativeFrame final {
 public:
  NativeFrame(Isolate& isolate, std::size_t root_count) noexcept;
  ~NativeFrame();
  NativeFrame(const NativeFrame&) = delete;
  NativeFrame& operator=(const NativeFrame&) = delete;
  NativeFrame(NativeFrame&&) = delete;
  NativeFrame& operator=(NativeFrame&&) = delete;

  [[nodiscard]] std::span<Value> roots() noexcept { return {roots_, root_count_}; }

 private:
  Value* roots_ = nullptr;
  std::size_t root_count_ = 0;
  RootRange root_range_;
};

// 每次执行独占；不改变宿主 NativeContext 的生命周期和责任。
// Isolate 必须晚于此对象销毁，所有操作发生在 Isolate 所属线程。
class NativeExecution final {
 public:
  explicit NativeExecution(Isolate& isolate) noexcept : isolate_(&isolate) {}
  ~NativeExecution();
  NativeExecution(const NativeExecution&) = delete;
  NativeExecution& operator=(const NativeExecution&) = delete;
  NativeExecution(NativeExecution&&) = delete;
  NativeExecution& operator=(NativeExecution&&) = delete;

  [[nodiscard]] ExecutionStatus Enter(std::size_t root_count) noexcept;
  void Exit() noexcept;
  [[nodiscard]] std::size_t frame_count() const noexcept { return frame_count_; }
  [[nodiscard]] std::span<Value> current_roots() noexcept;

 private:
  Isolate* isolate_;
  std::array<std::unique_ptr<NativeFrame>, kMaxCallFrames> frames_;
  std::size_t frame_count_ = 0;
};

// 版本化符号使错误 ABI 的静态产物在链接时即被拒绝。
extern "C" std::uint8_t vela_native_v1_frame_enter(NativeExecution* execution,
                                                   std::uint64_t root_count) noexcept;
extern "C" void vela_native_v1_frame_exit(NativeExecution* execution) noexcept;

}  // namespace vela::runtime
