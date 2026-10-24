#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "vm/bytecode.h"

namespace vela::compiler {

// 固定长度的原生指针池同时作为 GC 根；Isolate 必须覆盖所有共享持有者的生命周期。
struct StringConstants final {
  StringConstants(runtime::Heap& heap, std::size_t count) : values(count), roots(heap, values) {}

  std::vector<runtime::StringObject*> values;
  runtime::RootRange roots;
};

struct CompiledFunction final {
  std::string name;
  std::vector<vm::Instruction> code;
  std::vector<runtime::ClassLayout*> classes;
  std::shared_ptr<const StringConstants> strings;
  std::vector<const runtime::ArrayLayout*> arrays;
  std::vector<double> doubles;
  std::uint8_t register_count = 0;
  std::uint16_t parameter_count = 0;

  [[nodiscard]] auto View() const noexcept -> vm::Function {
    return {
        .code = code,
        .classes = classes,
        .strings = strings != nullptr ? std::span<runtime::StringObject* const>(strings->values)
                                      : std::span<runtime::StringObject* const>(),
        .arrays = arrays,
        .doubles = doubles,
        .register_count = register_count,
        .parameter_count = static_cast<std::uint8_t>(parameter_count),
    };
  }
};

struct CompiledProgram final {
  CompiledProgram() = default;
  ~CompiledProgram() = default;
  CompiledProgram(const CompiledProgram& other);
  auto operator=(const CompiledProgram& other) -> CompiledProgram&;
  CompiledProgram(CompiledProgram&&) noexcept = default;
  auto operator=(CompiledProgram&&) noexcept -> CompiledProgram& = default;

  std::vector<CompiledFunction> functions;
  std::vector<vm::Function> views;

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
