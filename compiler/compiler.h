#pragma once

#include <span>
#include <string_view>

#include "compiler/compilation_state.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/type_checker.h"
#include "runtime/isolate.h"
#include "vm/program.h"

namespace vela::compiler {

struct CompileResult final {
  vm::Program program;
  frontend::Diagnostics diagnostics;

  [[nodiscard]] bool Succeeded() const noexcept { return diagnostics.empty(); }
};

[[nodiscard]] CompileResult CompileSource(std::string_view source, runtime::Isolate& isolate);
[[nodiscard]] CompileResult CompileModules(std::span<const frontend::SourceInput> sources,
                                           std::string_view entry,
                                           runtime::Isolate& isolate);
[[nodiscard]] CompileResult CompileFile(std::string_view path, runtime::Isolate& isolate);
[[nodiscard]] CompileResult CompileParsed(frontend::ParseResult parsed, runtime::Isolate& isolate);

// state 是调用方尚未提交的工作副本；失败时丢弃，成功后才允许发布。
[[nodiscard]] CompileResult CompileProgram(const frontend::Program& source,
                                           const frontend::SemanticModel& model,
                                           runtime::Isolate& isolate,
                                           CompilationState* state = nullptr);

}  // namespace vela::compiler
