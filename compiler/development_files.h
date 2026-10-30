#pragma once

#include <expected>
#include <string>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/source_session.h"

namespace vela::compiler {

// 文件监视器的解析缓存只按内容复用不可变语法，不保存已提交身份或运行时地址。
// Capture 固定本次全部模块文本；后续编译不得再次读取磁盘。
class DevelopmentFiles final {
 public:
  explicit DevelopmentFiles(std::string entry);
  [[nodiscard]] std::expected<SourceUpdate, frontend::Diagnostics> Capture(
      const CompiledSourceSnapshot& previous);
  // 首次成功 Capture 后是规范化的模块 URI，创建源码会话时使用同一身份。
  [[nodiscard]] const std::string& entry() const noexcept { return entry_; }

 private:
  std::string entry_;
  frontend::ModuleCache cache_;
};

}  // namespace vela::compiler
