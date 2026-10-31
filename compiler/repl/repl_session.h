#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "compiler/bytecode/compilation_state.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/repl/repl_source.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::compiler {

struct CompileResult;

struct SubmissionResult final {
  SubmissionStatus status = SubmissionStatus::Empty;
  frontend::Diagnostics diagnostics;
  vm::ExecutionResult execution;
};

// 会话独占 isolate 和最新程序；返回值中的引用只借用至下次提交或 Reset。
class ReplSession final {
 public:
  explicit ReplSession(const std::filesystem::path& directory,
                       std::size_t gc_threshold = runtime::kDefaultGcThreshold,
                       frontend::DiagnosticOptions options = {});
  ~ReplSession() = default;
  ReplSession(const ReplSession&) = delete;
  ReplSession& operator=(const ReplSession&) = delete;
  ReplSession(ReplSession&&) = delete;
  ReplSession& operator=(ReplSession&&) = delete;

  [[nodiscard]] SubmissionResult Submit(std::string_view source);
  [[nodiscard]] std::expected<void, std::vector<runtime::ExternalResourceInfo>> Reset();
  [[nodiscard]] runtime::Isolate& isolate() noexcept { return *isolate_; }
  [[nodiscard]] std::string_view source() const noexcept { return source_.source(); }
  [[nodiscard]] std::size_t function_count() const noexcept { return program_.functions().size(); }

 private:
  [[nodiscard]] CompileResult Compile(ReplSourceCandidate& prepared,
                                      CompilationState& compilation,
                                      frontend::ModuleCache& modules);

  std::filesystem::path path_;
  const std::size_t gc_threshold_;
  frontend::DiagnosticOptions options_;
  std::optional<runtime::NativeBindings> bindings_;
  std::unique_ptr<runtime::Isolate> isolate_;
  std::unique_ptr<vm::VirtualMachine> machine_;
  vm::Program program_;
  CompilationState compilation_;
  frontend::ModuleCache modules_;
  ReplSource source_;
};

}  // namespace vela::compiler
