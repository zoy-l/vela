#pragma once

#include "compiler/ir/ir.h"

namespace vela::compiler::aot {

void BindNativeReflectedMembers(ir::Program& program);

}  // namespace vela::compiler::aot
