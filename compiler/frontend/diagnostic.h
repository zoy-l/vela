#pragma once

#include <algorithm>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/source_location.h"

namespace vela::frontend {

using ::vela::SourceLocation;

enum class DiagnosticKind : std::uint8_t { Source, Io, Internal };
enum class DiagnosticSeverity : std::uint8_t { Warning, Error };
enum class DiagnosticCode : std::uint8_t { None, ImplicitDynamicConversion };

struct DiagnosticOptions final {
  DiagnosticSeverity implicit_dynamic_conversion = DiagnosticSeverity::Warning;
};

struct Diagnostic final {
  SourceLocation location;
  std::string message;
  std::string source_path;
  DiagnosticKind kind = DiagnosticKind::Source;
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  DiagnosticCode code = DiagnosticCode::None;
};

using Diagnostics = std::vector<Diagnostic>;

[[nodiscard]] inline bool HasErrors(std::span<const Diagnostic> diagnostics) {
  return std::ranges::any_of(diagnostics, [](const Diagnostic& diagnostic) {
    return diagnostic.severity == DiagnosticSeverity::Error;
  });
}

[[nodiscard]] std::string_view DiagnosticCodeName(DiagnosticCode code);
void WriteDiagnostic(std::ostream& output,
                     const Diagnostic& diagnostic,
                     std::string_view default_path = {});
[[nodiscard]] std::string DiagnosticsText(std::span<const Diagnostic> diagnostics);

}  // namespace vela::frontend
