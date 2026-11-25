#pragma once

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"

namespace vela::compiler {

// 文件会话固定本次模块文本；编译不得再次读取磁盘。未接纳的编辑保持 dirty，
// 直到提交或被后续编辑替换。实现分别拥有本地订阅或进程外会话，不共享实现状态。
class DevelopmentFiles {
 public:
  [[nodiscard]] static std::unique_ptr<DevelopmentFiles> Create(std::string entry,
                                                                frontend::ModulePaths imports = {});
  DevelopmentFiles(const DevelopmentFiles&) = delete;
  DevelopmentFiles& operator=(const DevelopmentFiles&) = delete;
  DevelopmentFiles(DevelopmentFiles&&) = delete;
  DevelopmentFiles& operator=(DevelopmentFiles&&) = delete;
  virtual ~DevelopmentFiles() = default;
  [[nodiscard]] virtual std::expected<SourceUpdate, frontend::Diagnostics> Capture(
      const CompiledSourceSnapshot& previous) = 0;
  // 通知只唤醒消费者；没有文件变化时不定时轮询。
  [[nodiscard]] virtual std::expected<void, frontend::Diagnostics> StartWatching(
      std::function<void()> changed = {}) = 0;
  [[nodiscard]] virtual std::expected<void, frontend::Diagnostics> WatchSources(
      std::span<const SourceChange> changes) = 0;
  // 接纳较早候选不会覆盖其后已捕获的未提交文本。
  [[nodiscard]] virtual std::expected<void, frontend::Diagnostics> Accept(
      std::shared_ptr<const CompiledSourceSnapshot> snapshot) = 0;
  [[nodiscard]] virtual std::expected<std::optional<SourceUpdate>, frontend::Diagnostics> Poll(
      std::chrono::milliseconds timeout = {}) = 0;
  // 首次成功 Capture 后返回规范化模块 URI，源码会话使用同一身份。
  [[nodiscard]] virtual std::string entry() const = 0;

 protected:
  DevelopmentFiles() = default;
};

}  // namespace vela::compiler
