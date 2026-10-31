#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/source_location.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"

namespace vela::compiler {

enum class SubmissionStatus : std::uint8_t { Empty, Incomplete, Rejected, Executed, RuntimeError };

struct ReplSourceError final {
  SubmissionStatus status = SubmissionStatus::Empty;
  frontend::Diagnostics diagnostics;
};

struct ReplEntry final {
  std::uint32_t header_line = 0;
  std::uint32_t first_line = 0;
  std::uint32_t last_line = 0;
  std::uint32_t submission = 0;
  bool wrapper = false;
  [[nodiscard]] std::string name() const;
};

struct ReplSourceCandidate final {
  ReplEntry entry;
  std::string input;
  std::shared_ptr<const frontend::Program> addition;
  frontend::ParsedSource parsed;
};

// VM 与 JIT 复用的追加源码事务；仅在后端接受后 Commit，历史语句不会成为新入口。
class ReplSource final {
 public:
  [[nodiscard]] std::expected<ReplSourceCandidate, ReplSourceError> Prepare(
      std::string_view source) const;
  void Commit(ReplSourceCandidate& candidate);
  void Remap(frontend::Diagnostics& diagnostics, const ReplEntry& current) const;
  void Remap(SourceLocation& location, std::string& path, const ReplEntry& current) const;
  [[nodiscard]] std::string_view source() const noexcept { return user_source_; }

 private:
  enum class Echo : std::uint8_t { Display, Suppress };
  void BindEntries(frontend::Program& program, const ReplEntry& current, Echo echo) const;
  [[nodiscard]] ReplEntry Locate(std::uint32_t line, const ReplEntry& current) const;

  std::vector<std::shared_ptr<const frontend::Program>> syntax_;
  std::shared_ptr<const frontend::SourceSnapshot> history_;
  std::string user_source_;
  std::vector<ReplEntry> entries_;
  std::uint32_t next_line_ = 1;
};

}  // namespace vela::compiler
