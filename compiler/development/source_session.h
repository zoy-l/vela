#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/declaration.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/ir/ir.h"

namespace vela::compiler {

struct SourceChange final {
  std::string path;
  // 空值表示删除模块；空字符串仍是合法的空源码。
  std::optional<std::string> text;
  // 交互源码携带解析器生成并标注入口角色的不可变树；必须与 text 相同。
  std::shared_ptr<const frontend::ParsedSource> syntax = nullptr;
};

struct SourceUpdate final {
  std::uint64_t base_revision = 0;
  std::vector<SourceChange> changes;
  std::vector<std::string> added_roots;
};

struct SourceTimings final {
  std::chrono::nanoseconds loading{};
  std::chrono::nanoseconds checking{};
  std::chrono::nanoseconds ir{};
};

struct CompiledSourceSnapshot final {
  std::uint64_t revision = 0;
  absl::flat_hash_map<std::string, std::shared_ptr<const std::string>> sources;
  absl::flat_hash_set<std::string> prepared_sources;
  std::vector<std::string> roots;
  std::vector<std::string> standard_names;
  frontend::DeclarationIds identities;
  frontend::ModuleCache modules;
  ir::Program program;
  SourceTimings timings;
  frontend::Diagnostics diagnostics;
};

enum class SourceCandidateStatus : std::uint8_t {
  SourceError,
  Stale,
  DifferentSession,
  RequiresReload,
};

struct SourceCandidateError final {
  SourceCandidateStatus status = SourceCandidateStatus::SourceError;
  frontend::Diagnostics diagnostics;
  DeclarationId declaration;
};

struct SourceSessionIdentity final {};

class SourceCandidate final {
 public:
  [[nodiscard]] const CompiledSourceSnapshot& snapshot() const noexcept { return *prepared_; }

 private:
  friend class SourceSession;
  SourceCandidate(std::shared_ptr<const SourceSessionIdentity> session,
                  std::shared_ptr<const CompiledSourceSnapshot> base,
                  std::shared_ptr<const CompiledSourceSnapshot> prepared);
  std::shared_ptr<const SourceSessionIdentity> session_;
  std::shared_ptr<const CompiledSourceSnapshot> base_;
  std::shared_ptr<const CompiledSourceSnapshot> prepared_;
};

// 编译层只拥有不可变源码和声明身份，不访问 Isolate。代码生成或运行时验证成功后，
// 开发会话才提交候选；失败候选和并行编译不会改变已提交基线。
class SourceSession final {
 public:
  explicit SourceSession(const std::string& entry, frontend::DiagnosticOptions options = {});
  [[nodiscard]] std::shared_ptr<const CompiledSourceSnapshot> snapshot() const noexcept;
  [[nodiscard]] std::expected<SourceCandidate, SourceCandidateError> Prepare(
      SourceUpdate update,
      std::span<const DeclarationId> retained = {}) const;
  [[nodiscard]] std::expected<void, SourceCandidateStatus> Commit(const SourceCandidate& candidate);

 private:
  const std::shared_ptr<const SourceSessionIdentity> identity_;
  frontend::DiagnosticOptions options_;
  mutable std::mutex mutex_;
  std::shared_ptr<const CompiledSourceSnapshot> current_;
};

}  // namespace vela::compiler
