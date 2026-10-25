#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "base/source_location.h"
#include "runtime/value.h"

namespace vela::vm {
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
  [[nodiscard]] auto name() const noexcept -> std::string_view;
  [[nodiscard]] auto instruction() const noexcept -> std::size_t { return instruction_; }
  [[nodiscard]] auto location() const noexcept -> std::optional<DebugLocation>;
  [[nodiscard]] auto register_count() const noexcept -> std::size_t;
  [[nodiscard]] auto ReadRegister(std::size_t index) const noexcept
      -> std::optional<runtime::Value>;

 private:
  friend class DebugContext;
  DebugFrame(const internal::ActiveFrame& frame, std::size_t instruction) noexcept
      : frame_(&frame), instruction_(instruction) {}
  const internal::ActiveFrame* frame_;
  std::size_t instruction_;
};

class DebugContext final {
 public:
  [[nodiscard]] auto frame_count() const noexcept -> std::size_t { return frame_count_; }
  [[nodiscard]] auto current_frame() const noexcept -> DebugFrame;
  // depth 0 为当前帧；调用者的位置指向调用指令而不是返回后的下一条指令。
  [[nodiscard]] auto Frame(std::size_t depth) const noexcept -> std::optional<DebugFrame>;

 private:
  friend class internal::Execution;
  DebugContext(const internal::ActiveFrame* frames, std::size_t count) noexcept
      : frames_(frames), frame_count_(count) {}
  // ActiveFrame 在此是不完整类型；MSVC span 会提前实例化迭代器，实际 span 在实现中构造。
  const internal::ActiveFrame* frames_;
  std::size_t frame_count_;
};

enum class DebugAction : std::uint8_t { Continue, Stop };

class Debugger {
 public:
  Debugger() = default;
  virtual ~Debugger() = default;
  Debugger(const Debugger&) = delete;
  auto operator=(const Debugger&) -> Debugger& = delete;
  Debugger(Debugger&&) = delete;
  auto operator=(Debugger&&) -> Debugger& = delete;
  virtual auto BeforeInstruction(const DebugContext& context) noexcept -> DebugAction = 0;
};

}  // namespace vela::vm
