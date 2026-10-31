#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "base/source_location.h"

namespace vela::runtime {

class NativeControl;
class NativeExecution;
struct NativeTraceFrame;
struct NativeImage;
struct NativeFunctionDescription;
struct NativeCheckpoint;

// 只在调试安全点回调中借用；源码来自真实帧的不可变版本，不重新读取磁盘或当前快照。
class NativeDebugFrame final {
 public:
  [[nodiscard]] FunctionIdentity identity() const noexcept;
  [[nodiscard]] std::string_view name() const noexcept;
  [[nodiscard]] std::string_view source_path() const noexcept;
  [[nodiscard]] std::string_view source_text() const noexcept;
  [[nodiscard]] std::optional<SourceLocation> location() const noexcept { return location_; }
  [[nodiscard]] std::uint32_t instruction() const noexcept { return instruction_; }
  [[nodiscard]] NativeCodeTier tier() const noexcept;

 private:
  friend class NativeDebugContext;
  explicit NativeDebugFrame(const NativeTraceFrame& frame) noexcept;
  const NativeImage* image_;
  const NativeFunctionDescription* function_;
  std::optional<SourceLocation> location_;
  std::uint32_t instruction_;
  const NativeCheckpoint* checkpoint_;
};

class NativeDebugContext final {
 public:
  [[nodiscard]] const NativeDebugFrame& top_frame() const noexcept { return top_frame_; }
  [[nodiscard]] std::span<const NativeDebugFrame> frames() const;
  // 与 Reload 使用相同的停止状态逐层退出，不把停止变成可捕获脚本异常。
  void RequestStop() const noexcept;

 private:
  friend class NativeControl;
  explicit NativeDebugContext(NativeExecution& execution);
  NativeExecution* execution_;
  const NativeTraceFrame* first_frame_;
  NativeDebugFrame top_frame_;
  mutable std::vector<NativeDebugFrame> frames_;
};

class NativeDebugger {
 public:
  NativeDebugger() = default;
  virtual ~NativeDebugger() = default;
  NativeDebugger(const NativeDebugger&) = delete;
  NativeDebugger& operator=(const NativeDebugger&) = delete;
  NativeDebugger(NativeDebugger&&) = delete;
  NativeDebugger& operator=(NativeDebugger&&) = delete;
  virtual void AtSafepoint(const NativeDebugContext& context) noexcept = 0;
};

}  // namespace vela::runtime
