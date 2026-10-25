#pragma once

#include <string>
#include <vector>

#include "base/source_location.h"

namespace vela::frontend {

using ::vela::SourceLocation;

struct Diagnostic final {
  SourceLocation location;
  std::string message;
  std::string source_path;
};

using Diagnostics = std::vector<Diagnostic>;

}  // namespace vela::frontend
