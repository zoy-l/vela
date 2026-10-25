#pragma once

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"
#include "runtime/isolate.h"

namespace vela::compiler {

[[nodiscard]] frontend::Diagnostics CheckNativeBindings(const frontend::Program& program,
                                                        const frontend::SemanticModel& model,
                                                        const runtime::Isolate& isolate);

}  // namespace vela::compiler
