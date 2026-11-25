#include <expected>
#include <memory>
#include <string>
#include <utility>
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/engine.h"

#include "compiler/development/source_session.h"

namespace vela::compiler::jit {

Compiler::Compiler(std::shared_ptr<CodeLinker> jit)
    : jit_(std::move(jit)), counters_(std::make_shared<CodeResourceCounters>()) {
  InstallResourceAccounting(*jit_, counters_);
}

CodeResourceCounts Compiler::resource_counts() const noexcept {
  return counters_->Snapshot();
}

frontend::Diagnostics CompilationDiagnostic(std::string message) {
  return {{.location = {}, .message = std::move(message), .source_path = {}}};
}

std::expected<CompiledImage, frontend::Diagnostics> Compiler::CompileSource(
    const std::shared_ptr<const CompiledSourceSnapshot>& source,
    const CompileOptions& options,
    CodeReuseCache* reuse) {
  return Compile(source->program, options, reuse);
}

}  // namespace vela::compiler::jit
