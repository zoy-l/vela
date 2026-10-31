#pragma once

#include <span>
#include <string_view>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/jit/compiler.h"

namespace vela::compiler::jit {

struct SourceOptions final {
  frontend::DiagnosticOptions diagnostics;
  CompileOptions compilation;
};

struct SourceResult final {
  CompiledImage code;
  frontend::Diagnostics diagnostics;

  [[nodiscard]] bool Succeeded() const noexcept { return code.image != nullptr; }
};

// 普通执行编译封闭源码快照，产物独立于编译器及 AST，可实例化到不同 Isolate。
// 可重载会话继续由 SourceSession 维护声明身份、版本及发布事务。
[[nodiscard]] SourceResult CompileParsed(Compiler& compiler,
                                         frontend::ParseResult parsed,
                                         const SourceOptions& options = {});
[[nodiscard]] SourceResult CompileSource(Compiler& compiler,
                                         std::string_view source,
                                         const SourceOptions& options = {});
[[nodiscard]] SourceResult CompileModules(Compiler& compiler,
                                          std::span<const frontend::SourceInput> sources,
                                          std::string_view entry,
                                          const SourceOptions& options = {});
[[nodiscard]] SourceResult CompileFile(Compiler& compiler,
                                       std::string_view path,
                                       const SourceOptions& options = {});

}  // namespace vela::compiler::jit
