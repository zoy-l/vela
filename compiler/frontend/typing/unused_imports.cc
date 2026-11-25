#include "compiler/frontend/typing/unused_imports.h"

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

Diagnostics UnusedImportDiagnostics(const Program& program) {
  Diagnostics diagnostics;
  for (const auto& imported : program.unused_imports) {
    const auto& source = program.sources[imported.location.source_id];
    diagnostics.push_back({
        .location = imported.diagnostic_location,
        .message = "unused import '" + imported.path + "'",
        .source_path = source.path,
        .severity = DiagnosticSeverity::Warning,
        .code = DiagnosticCode::UnusedImport,
        .length = imported.diagnostic_length,
    });
  }
  return diagnostics;
}

}  // namespace vela::frontend
