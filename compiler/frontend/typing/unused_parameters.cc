#include "compiler/frontend/typing/unused_parameters.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

void ParameterUsage::Declare(const Parameter& parameter, std::uint32_t binding) {
  parameters_.emplace(binding, ParameterReads{
                                   .name = parameter.name,
                                   .location = parameter.location,
                                   .reads = {},
                               });
}

void ParameterUsage::Read(const Expression& expression, std::uint32_t binding) {
  if (const auto found = parameters_.find(binding); found != parameters_.end()) {
    found->second.reads.insert(&expression);
  }
}

void ParameterUsage::Write(const Expression& expression, std::uint32_t binding) {
  if (const auto found = parameters_.find(binding); found != parameters_.end()) {
    found->second.reads.erase(&expression);
  }
}

Diagnostics ParameterUsage::DiagnosticsFor(const Program& program) const {
  Diagnostics diagnostics;
  for (const auto& [binding, parameter] : parameters_) {
    if (!parameter.reads.empty() || parameter.name.starts_with('_')) {
      continue;
    }
    diagnostics.push_back({
        .location = parameter.location,
        .message = "parameter '" + parameter.name + "' is never read",
        .source_path = program.sources[parameter.location.source_id].path,
        .severity = DiagnosticSeverity::Warning,
        .code = DiagnosticCode::UnusedParameter,
        .length = parameter.name.size(),
    });
  }
  std::ranges::sort(diagnostics, {}, [](const Diagnostic& diagnostic) {
    const auto& location = diagnostic.location;
    return std::tie(location.source_id, location.line, location.column);
  });
  return diagnostics;
}

}  // namespace vela::frontend
