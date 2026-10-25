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

  [[nodiscard]] auto Succeeded() const noexcept -> bool { return diagnostics.empty(); }
};

[[nodiscard]] auto CompileSource(std::string_view source, runtime::Isolate& isolate)
    -> CompileResult;
[[nodiscard]] auto CompileModules(std::span<const frontend::SourceInput> sources,
                                  std::string_view entry,
                                  runtime::Isolate& isolate) -> CompileResult;
[[nodiscard]] auto CompileFile(std::string_view path, runtime::Isolate& isolate) -> CompileResult;
[[nodiscard]] auto CompileParsed(frontend::ParseResult parsed, runtime::Isolate& isolate)
    -> CompileResult;

// state 是调用方尚未提交的工作副本；失败时丢弃，成功后才允许发布。
[[nodiscard]] auto CompileProgram(const frontend::Program& source,
                                  const frontend::SemanticModel& model,
                                  runtime::Isolate& isolate,
                                  CompilationState* state = nullptr) -> CompileResult;

}  // namespace vela::compiler
