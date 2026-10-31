#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/declaration.h"
#include "runtime/native/native_debugger.h"

namespace vela::compiler {
class DevelopmentSession;
}

namespace vela::tooling {

enum class DebugCommandResult : std::uint8_t { Handled, Unknown };

// 只在 Isolate 所属线程操作。暂停期间借用真实帧，继续后不保留源码或代码版本引用。
class DevelopmentDebugger final : public runtime::NativeDebugger {
 public:
  DevelopmentDebugger(compiler::DevelopmentSession& session,
                      std::ostream& errors,
                      std::function<void()> poll);
  ~DevelopmentDebugger() override;
  DevelopmentDebugger(const DevelopmentDebugger&) = delete;
  DevelopmentDebugger& operator=(const DevelopmentDebugger&) = delete;
  DevelopmentDebugger(DevelopmentDebugger&&) = delete;
  DevelopmentDebugger& operator=(DevelopmentDebugger&&) = delete;
  [[nodiscard]] DebugCommandResult Command(std::string_view command);
  void AtSafepoint(const runtime::NativeDebugContext& context) noexcept override;

 private:
  struct Position final {
    FunctionIdentity identity;
    std::uint32_t instruction = 0;
    std::size_t depth = 0;
    bool operator==(const Position&) const = default;
  };
  struct Breakpoint final {
    std::string path;
    std::uint32_t line = 0;
  };
  struct BreakLocation final {
    FunctionIdentity identity;
    std::uint32_t line = 0;
    std::uint64_t generation = 0;
    bool operator==(const BreakLocation&) const = default;
  };
  void Break(std::string_view arguments);
  void Stack() const;
  void Source(std::string_view argument) const;
  void PrintFrame(std::size_t index, const runtime::NativeDebugFrame& frame) const;
  [[nodiscard]] bool IsBreakpoint(const runtime::NativeDebugFrame& frame) const;

  compiler::DevelopmentSession* session_;
  std::ostream* errors_;
  std::function<void()> poll_;
  const runtime::NativeDebugContext* paused_ = nullptr;
  std::vector<Breakpoint> breakpoints_;
  std::optional<Position> step_from_;
  std::optional<BreakLocation> last_break_;
  bool pause_requested_ = false;
  bool continuing_ = false;
};

}  // namespace vela::tooling
