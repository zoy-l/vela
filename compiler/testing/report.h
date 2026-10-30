#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "compiler/testing/result.h"
#include "compiler/testing/settings.h"
#include "runtime/future.h"

namespace vela::testing {

enum class ReportEventType : std::uint8_t {
  Start,
  Suite,
  Group,
  Test,
  TestStart,
  Print,
  Warning,
  Error,
  TestDone,
  Done,
};

// worker 只发布拥有文本和数值的事件，不跨 Isolate 传递 Value 或借用的源码数据。
struct ReportEvent final {
  explicit ReportEvent(ReportEventType type, std::uint64_t suite = 0) : type(type), suite(suite) {}
  ReportEventType type;
  std::uint64_t suite = 0;
  std::optional<std::uint64_t> test;
  std::optional<std::uint64_t> scope;
  std::optional<std::uint64_t> hook;
  std::optional<std::uint64_t> parent;
  std::string file;
  std::string name;
  std::string text;
  std::vector<std::string> tags;
  std::string skip_reason;
  std::vector<runtime::FutureTraceFrame> trace;
  std::optional<TestError> error;
  std::optional<TestStatus> status;
  std::uint64_t passed = 0;
  std::uint64_t failed = 0;
  std::uint64_t skipped = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t selected = 0;
  bool listed = false;
  bool success = false;
};

using ReportSink = std::function<void(const ReportEvent&)>;
enum class TerminalMode : std::uint8_t { Plain, Interactive };

// 一个会话只使用一个 reporter；互斥边界统一分配事件序号和单调相对时间。
class SessionReporter final {
 public:
  SessionReporter(Reporter kind, std::ostream& output, TerminalMode mode = TerminalMode::Plain);
  void Emit(const ReportEvent& event);
  [[nodiscard]] bool HasFailed() const noexcept { return failed_.load(std::memory_order_relaxed); }

 private:
  struct Stamp final {
    std::uint64_t sequence;
    double milliseconds;
  };
  void WriteJson(const ReportEvent& event, Stamp stamp);
  void WriteHuman(const ReportEvent& event);
  Reporter kind_;
  std::ostream* output_;
  std::mutex mutex_;
  std::uint64_t sequence_ = 0;
  std::chrono::steady_clock::time_point started_;
  std::atomic_bool failed_{false};
  TerminalMode mode_;
  std::uint64_t completed_ = 0;
  bool progress_visible_ = false;
};

}  // namespace vela::testing
