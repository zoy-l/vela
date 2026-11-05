#pragma once

#include "base/layout_description.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

[[nodiscard]] layout::Program DescribeLayouts(const Program& program,
                                              const frontend::SemanticModel& model,
                                              const frontend::Program& source);

}  // namespace vela::ir
