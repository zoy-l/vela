#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "base/source_location.h"
#include "runtime/value.h"

namespace vela::vm {
template <class T>
class FrameSlots;
namespace internal {
struct ActiveFrame;
class Execution;
}  // namespace internal

struct DebugLocation final {
  std::string_view path;
  SourceLocation position;
};

// 只读视图仅在 BeforeInstruction 回调内有效；对象仍由暂停的真实帧精确保根。
class DebugFrame final {
 public:
  [[nodiscard]] std::string_view name() const noexcept;
  [[nodiscard]] std::size_t instruction() const noexcept { return instruction_; }
  [[nodiscard]] std::optional<DebugLocation> location() const noexcept;
  [[nodiscard]] std::size_t register_count() const noexcept;
  [[nodiscard]] std::optional<runtime::Value> ReadRegister(std::size_t index) const noexcept;

 private:
  friend class DebugContext;
  DebugFrame(const internal::ActiveFrame& frame, std::size_t instruction) noexcept
      : frame_(&frame), instruction_(instruction) {}
  const internal::ActiveFrame* frame_;
  std::size_t instruction_;
};

class DebugContext final {
 public:
  [[nodiscard]] std::size_t frame_count() const noexcept { return frame_count_; }
  [[nodiscard]] DebugFrame current_frame() const noexcept;
  // depth 0 为当前帧；调用者的位置指向调用指令而不是返回后的下一条指令。
  [[nodiscard]] std::optional<DebugFrame> Frame(std::size_t depth) const noexcept;

 private:
  friend class internal::Execution;
  DebugContext(const FrameSlots<internal::ActiveFrame>& frames, std::size_t count) noexcept
      : frames_(&frames), frame_count_(count) {}
  const FrameSlots<internal::ActiveFrame>* frames_;
  std::size_t frame_count_;
};

enum class DebugAction : std::uint8_t { Continue, Stop };

class Debugger {
 public:
  Debugger() = default;
  virtual ~Debugger() = default;
  Debugger(const Debugger&) = delete;
  Debugger& operator=(const Debugger&) = delete;
  Debugger(Debugger&&) = delete;
  Debugger& operator=(Debugger&&) = delete;
  virtual DebugAction BeforeInstruction(const DebugContext& context) noexcept = 0;
};

}  // namespace vela::vm
