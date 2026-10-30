#include "compiler/frontend/diagnostic.h"

#include <ostream>
#include <string_view>

namespace vela::frontend {

std::string_view DiagnosticCodeName(DiagnosticCode code) {
  switch (code) {
    case DiagnosticCode::None:
      return {};
    case DiagnosticCode::ImplicitDynamicConversion:
      return "implicit_dynamic_conversion";
  }
  return {};
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

}  // namespace vela::frontend
