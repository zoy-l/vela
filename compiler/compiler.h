#pragma once

#include <string_view>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
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
[[nodiscard]] auto CompileSources(const std::vector<std::string_view>& sources,
                                  runtime::Isolate& isolate) -> CompileResult;
[[nodiscard]] auto CompileFile(std::string_view path, runtime::Isolate& isolate) -> CompileResult;

[[nodiscard]] auto CompileProgram(const frontend::Program& source,
                                  const frontend::SemanticModel& model,
                                  runtime::Isolate& isolate) -> CompileResult;

}  // namespace vela::compiler
