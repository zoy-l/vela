#pragma once

#include <optional>

#include "compiler/development/development_session.h"
#include "runtime/native/native_debugger.h"

namespace vela::compiler::test {

// 测试和基准在 checkpoint 安全点消费一次候选；结果保留给调用方验证发布及迁移契约。
struct CheckpointPublication final : runtime::NativeDebugger {
  explicit CheckpointPublication(DevelopmentSession& session) : session_(session) {}
  void AtSafepoint(const runtime::NativeDebugContext& context) noexcept override;
  // 行断点或故障注入场景确认安全点后，直接消费候选，不改变其检查/注入时序。
  void PublishPending();
  void Stage(DevelopmentCandidate candidate);
  [[nodiscard]] bool HasPending() const { return pending_.has_value(); }
  [[nodiscard]] const DevelopmentResult& result() const { return result_; }

 private:
  DevelopmentSession& session_;
  std::optional<DevelopmentCandidate> pending_;
  DevelopmentResult result_;
};

}  // namespace vela::compiler::test
