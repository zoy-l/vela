#include "compiler/jit/source_compiler.h"

#include <iterator>
#include <span>
#include <string_view>
#include <utility>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/ir/preparation.h"
#include "compiler/jit/compiler.h"

namespace vela::compiler::jit {

SourceResult CompileParsed(Compiler& compiler,
                            frontend::ParseResult parsed,
                            const SourceOptions& options) {
  auto prepared = ir::PrepareParsed(std::move(parsed), options.diagnostics);
  SourceResult result{.code = {}, .diagnostics = std::move(prepared.diagnostics)};
  if (frontend::HasErrors(result.diagnostics)) {
    return result;
  }
  auto compiled = compiler.Compile(prepared.program, options.compilation);
  if (!compiled) {
    auto& errors = compiled.error();
    result.diagnostics.insert(result.diagnostics.end(), std::make_move_iterator(errors.begin()),
                               std::make_move_iterator(errors.end()));
    return result;
  }
  result.code = std::move(*compiled);
  return result;
}

SourceResult CompileSource(Compiler& compiler,
                            std::string_view source,
                            const SourceOptions& options) {
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  return CompileModules(compiler, std::span(&input, 1), input.path, options);
}

SourceResult CompileModules(Compiler& compiler,
                             std::span<const frontend::SourceInput> sources,
                             std::string_view entry,
                             const SourceOptions& options) {
  return CompileParsed(compiler, frontend::LoadSources(sources, entry), options);
}

SourceResult CompileFile(Compiler& compiler,
                          std::string_view path,
                          const SourceOptions& options) {
  return CompileParsed(compiler, frontend::LoadProgram(path), options);
}

}  // namespace vela::compiler::jit
