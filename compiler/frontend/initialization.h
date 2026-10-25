#pragma once

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {

[[nodiscard]] Diagnostics CheckConstructorInitialization(const Class& owner,
                                                         const SemanticModel& model);

}  // namespace vela::frontend
