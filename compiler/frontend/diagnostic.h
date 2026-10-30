#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "base/source_location.h"

namespace vela::frontend {

using ::vela::SourceLocation;

enum class DiagnosticKind : std::uint8_t { Source, Io, Internal };

struct Diagnostic final {
  SourceLocation location;
  std::string message;
  std::string source_path;
  DiagnosticKind kind = DiagnosticKind::Source;
};

using Diagnostics = std::vector<Diagnostic>;

}  // namespace vela::frontend
