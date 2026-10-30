#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "base/execution_status.h"
#include "compiler/development_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/repl_source.h"
#include "compiler/source_session.h"
#include "runtime/future.h"

namespace vela::compiler {

struct DevelopmentSubmission final {
  SubmissionStatus status = SubmissionStatus::Empty;
  frontend::Diagnostics diagnostics;
  std::optional<ExecutionStatus> execution;
  std::optional<runtime::FutureDiagnostic> runtime_diagnostic;
  std::optional<DevelopmentResult> update;
};

// 借用开发会话，在所属线程且无活动脚本调用时提交；文件更新仍通过显式 Load/Hot Reload。
class DevelopmentRepl final {
 public:
  DevelopmentRepl(DevelopmentSession& session, const std::string& path);
  [[nodiscard]] DevelopmentSubmission Submit(std::string_view input);
  // 源码拥有者提供不可变更新；Reset 移除交互模块，复用文件编辑的 Reload 事务重建运行。
  [[nodiscard]] DevelopmentResult Reset(SourceUpdate update);
  [[nodiscard]] std::string_view source() const noexcept;

 private:
  void Synchronize();
  void Remap(frontend::Diagnostics& diagnostics, const ReplEntry& entry) const;
  void Remap(runtime::FutureDiagnostic& diagnostic, const ReplEntry& entry) const;
  [[nodiscard]] DevelopmentResult Publish(DevelopmentCandidate candidate);

  DevelopmentSession* session_;
  std::string path_;
  ReplSource source_;
  std::string accepted_text_;
};

}  // namespace vela::compiler
