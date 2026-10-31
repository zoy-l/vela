#pragma once

#include "base/layout_description.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

[[nodiscard]] layout::Program DescribeLayouts(const Program& program,
                                              const frontend::SemanticModel& model);

}  // namespace vela::ir
