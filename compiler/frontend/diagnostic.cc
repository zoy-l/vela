#include "compiler/frontend/diagnostic.h"

#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace vela::frontend {

std::string_view DiagnosticCodeName(DiagnosticCode code) {
  switch (code) {
    case DiagnosticCode::None:
      return {};
    case DiagnosticCode::ImplicitDynamicConversion:
      return "implicit_dynamic_conversion";
  }
  std::unreachable();
}

void WriteDiagnostic(std::ostream& output,
                     const Diagnostic& diagnostic,
                     std::string_view default_path) {
  const std::string_view path =
      diagnostic.source_path.empty() ? default_path : diagnostic.source_path;
  if (path.empty()) {
    output << "vela";
  } else {
    output << path << ':' << diagnostic.location.line << ':' << diagnostic.location.column;
  }
  output << (diagnostic.severity == DiagnosticSeverity::Warning ? ": warning: " : ": error: ");
  const auto code = DiagnosticCodeName(diagnostic.code);
  if (!code.empty()) {
    output << '[' << code << "] ";
  }
  output << diagnostic.message << '\n';
}

void WriteDiagnostics(std::ostream& output,
                      std::span<const Diagnostic> diagnostics,
                      std::string_view default_path) {
  for (const auto& diagnostic : diagnostics) {
    WriteDiagnostic(output, diagnostic, default_path);
  }
}

std::string DiagnosticsText(std::span<const Diagnostic> diagnostics) {
  std::ostringstream output;
  WriteDiagnostics(output, diagnostics);
  return output.str();
}

}  // namespace vela::frontend
