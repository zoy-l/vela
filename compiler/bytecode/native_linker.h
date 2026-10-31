#pragma once

#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {

[[nodiscard]] frontend::Diagnostics CheckNativeBindings(const ir::Program& program,
                                                        const runtime::Isolate& isolate);

}  // namespace vela::compiler
