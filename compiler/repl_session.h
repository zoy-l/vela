#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/compilation_state.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"
#include "runtime/isolate.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::compiler {

struct CompileResult;

enum class SubmissionStatus : std::uint8_t { Empty, Incomplete, Rejected, Executed, RuntimeError };

struct SubmissionResult final {
  SubmissionStatus status = SubmissionStatus::Empty;
  frontend::Diagnostics diagnostics;
  vm::ExecutionResult execution;
};

// 会话独占 isolate 和最新程序；返回值中的引用只借用至下次提交或 Reset。
class ReplSession final {
 public:
  explicit ReplSession(const std::filesystem::path& directory);
  ~ReplSession() = default;
  ReplSession(const ReplSession&) = delete;
  auto operator=(const ReplSession&) -> ReplSession& = delete;
  ReplSession(ReplSession&&) = delete;
  auto operator=(ReplSession&&) -> ReplSession& = delete;

  [[nodiscard]] auto Submit(std::string_view source) -> SubmissionResult;
  void Reset();
  [[nodiscard]] auto isolate() noexcept -> runtime::Isolate& { return *isolate_; }
  [[nodiscard]] auto source() const noexcept -> std::string_view { return user_source_; }
  [[nodiscard]] auto function_count() const noexcept -> std::size_t {
    return program_.functions().size();
  }

 private:
  struct Entry final {
    std::uint32_t header_line = 0;
    std::uint32_t first_line = 0;
    std::uint32_t last_line = 0;
    std::uint32_t submission = 0;
    bool wrapper = false;
  };
  struct Prepared final {
    Entry entry;
    std::string source;
    frontend::Program program;
  };
  enum class Echo : std::uint8_t { Display, Suppress };
  [[nodiscard]] auto Prepare(std::string_view source) -> std::expected<Prepared, SubmissionResult>;
  void BindEntries(frontend::Program& program, const Entry& current, Echo echo) const;
  [[nodiscard]] auto Compile(Prepared& prepared,
                             CompilationState& compilation,
                             frontend::ModuleCache& modules) -> CompileResult;
  void Remap(frontend::Diagnostics& diagnostics, const Entry& current) const;
  void Remap(vm::ExecutionResult& result, const Entry& current) const;
  [[nodiscard]] auto Locate(std::uint32_t line, const Entry& current) const -> Entry;

  std::filesystem::path path_;
  std::unique_ptr<runtime::Isolate> isolate_;
  std::unique_ptr<vm::VirtualMachine> machine_;
  vm::Program program_;
  CompilationState compilation_;
  frontend::ModuleCache modules_;
  std::string history_;
  std::string user_source_;
  std::vector<Entry> entries_;
  std::uint32_t next_line_ = 1;
};

}  // namespace vela::compiler
