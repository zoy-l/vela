#pragma once

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

[[nodiscard]] Diagnostics UnusedImportDiagnostics(const Program& program);

}  // namespace vela::frontend
