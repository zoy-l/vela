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
  std::uint8_t register_count = 0;
  std::uint16_t parameter_count = 0;

  [[nodiscard]] vm::Function View() const noexcept {
    return {.code = code,
            .classes = classes,
            .register_count = register_count,
            .parameter_count = static_cast<std::uint8_t>(parameter_count)};
  }
};

struct CompiledProgram final {
  std::vector<CompiledFunction> functions;
  std::vector<vm::Function> views;

  [[nodiscard]] const CompiledFunction* FindFunction(std::string_view name) const noexcept;
  [[nodiscard]] vm::Program View() const noexcept { return {.functions = views}; }
};

struct CompileResult final {
  CompiledProgram program;
  frontend::Diagnostics diagnostics;

  [[nodiscard]] bool Succeeded() const noexcept { return diagnostics.empty(); }
};

[[nodiscard]] CompileResult CompileSource(std::string_view source, runtime::Isolate& isolate);

[[nodiscard]] CompileResult CompileProgram(const frontend::Program& source,
                                           const frontend::SemanticModel& model,
                                           runtime::Isolate& isolate);

}  // namespace vela::compiler
