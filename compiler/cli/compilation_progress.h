#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <mutex>
#include <string_view>
#include <thread>

#include "compiler/frontend/modules/source_progress.h"

namespace vela::tooling {

enum class CompilationProgressOutput : std::uint8_t { Plain, Interactive };

// 状态输出只借用错误流；Finish 停止动画并在诊断或脚本输出前清除状态行。
class CompilationProgressDisplay final {
 public:
  explicit CompilationProgressDisplay(std::ostream& output);
  CompilationProgressDisplay(std::ostream& output, CompilationProgressOutput mode);
  ~CompilationProgressDisplay();
  CompilationProgressDisplay(const CompilationProgressDisplay&) = delete;
  CompilationProgressDisplay& operator=(const CompilationProgressDisplay&) = delete;
  CompilationProgressDisplay(CompilationProgressDisplay&&) = delete;
  CompilationProgressDisplay& operator=(CompilationProgressDisplay&&) = delete;

  void Report(const frontend::SourceProgress& progress);
  void Finish();

 private:
  void Animate();
  void Write(std::string_view text);
  void Clear();

  std::ostream& output_;
  CompilationProgressOutput mode_;
  std::filesystem::path directory_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool active_ = true;
  bool loading_ = true;
  std::size_t width_ = 0;
  std::size_t frame_ = 0;
  std::jthread animation_;
};

}  // namespace vela::tooling
