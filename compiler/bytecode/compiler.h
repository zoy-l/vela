#pragma once

#include <span>
#include <string_view>

#include "compiler/bytecode/compilation_state.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/typing/type_checker.h"
#include "runtime/isolate.h"
#include "vm/program.h"

namespace vela::compiler {

struct CompileResult final {
  vm::Program program;
  frontend::Diagnostics diagnostics;

  [[nodiscard]] bool Succeeded() const noexcept { return !frontend::HasErrors(diagnostics); }
};

[[nodiscard]] CompileResult CompileSource(std::string_view source,
                                          runtime::Isolate& isolate,
                                          frontend::DiagnosticOptions options = {});
[[nodiscard]] CompileResult CompileModules(std::span<const frontend::SourceInput> sources,
                                           std::string_view entry,
                                           runtime::Isolate& isolate,
                                           frontend::DiagnosticOptions options = {});
[[nodiscard]] CompileResult CompileFile(std::string_view path,
                                        runtime::Isolate& isolate,
                                        frontend::DiagnosticOptions options = {});
[[nodiscard]] CompileResult CompileParsed(frontend::ParseResult parsed,
                                          runtime::Isolate& isolate,
                                          frontend::DiagnosticOptions options = {});

// state 是调用方尚未提交的工作副本；失败时丢弃，成功后才允许发布。
[[nodiscard]] CompileResult CompileProgram(const frontend::Program& source,
                                           const frontend::SemanticModel& model,
                                           runtime::Isolate& isolate,
                                           CompilationState* state = nullptr);

}  // namespace vela::compiler
