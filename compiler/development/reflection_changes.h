#pragma once

#include "compiler/development/development_changes.h"
#include "compiler/ir/ir.h"

namespace vela::compiler {

void DescribeReflectionChanges(const ir::Program& previous,
                               const ir::Program& current,
                               DevelopmentChanges& changes);

}  // namespace vela::compiler
