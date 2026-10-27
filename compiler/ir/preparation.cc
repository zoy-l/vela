#include "compiler/ir/preparation.h"

#include <expected>
#include <utility>

#include "compiler/frontend/specialization.h"
#include "compiler/ir/layout_description.h"
#include "compiler/ir/optimizer.h"

namespace vela::ir {

LoweringResult PrepareProgram(const frontend::Program& source,
                              const frontend::SemanticModel& model,
                              PersistentIds* ids) {
  auto lowered = Lowerer(source, model, ids).Lower();
  if (!lowered.Succeeded()) {
    return lowered;
  }
  auto& program = lowered.program;
  program.source_paths.reserve(source.sources.size());
  for (const auto& file : source.sources) {
    program.source_paths.push_back(file.path);
  }
  program.layouts = DescribeLayouts(program, model);
  for (auto& function : program.functions) {
    if (function.native_symbol.empty()) {
      Optimize(function, program.functions);
    }
  }
  return lowered;
}

std::expected<Program, frontend::Diagnostics> PrepareParsed(frontend::ParseResult parsed) {
  if (!parsed.diagnostics.empty()) {
    return std::unexpected(std::move(parsed.diagnostics));
  }
  auto specialized = frontend::SpecializeGenerics(parsed.program);
  if (!specialized.empty()) {
    return std::unexpected(std::move(specialized));
  }
  auto checked = frontend::TypeChecker().Check(parsed.program);
  if (!checked.diagnostics.empty()) {
    return std::unexpected(std::move(checked.diagnostics));
  }
  auto lowered = PrepareProgram(parsed.program, checked.model);
  if (!lowered.Succeeded()) {
    return std::unexpected(std::move(lowered.diagnostics));
  }
  return std::move(lowered.program);
}

}  // namespace vela::ir
