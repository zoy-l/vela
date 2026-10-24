#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::compiler {

struct CompiledFunction final {
  std::string name;
  std::vector<vm::Instruction> code;
  std::vector<runtime::ClassLayout*> classes;
  std::vector<runtime::StringObject*> strings;
  std::vector<const runtime::ArrayLayout*> arrays;
  std::vector<double> doubles;
  std::uint8_t register_count = 0;
  std::uint16_t parameter_count = 0;

  [[nodiscard]] auto View() const noexcept -> vm::Function {
    return {
        .code = code,
        .classes = classes,
        .strings = strings,
        .arrays = arrays,
        .doubles = doubles,
        .register_count = register_count,
        .parameter_count = static_cast<std::uint8_t>(parameter_count),
    };
  }
};

struct CompiledProgram final {
  std::vector<CompiledFunction> functions;
  std::vector<vm::Function> views;
  std::vector<runtime::StringObject*> strings;

  [[nodiscard]] auto FindFunction(std::string_view name) const noexcept -> const CompiledFunction*;
  [[nodiscard]] auto View() const noexcept -> vm::Program { return {.functions = views}; }
};

struct CompileResult final {
  CompiledProgram program;
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
