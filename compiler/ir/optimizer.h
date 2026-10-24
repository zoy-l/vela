#pragma once

#include <span>

#include "compiler/ir/ir.h"

namespace vela::ir {

void Optimize(Function& function, std::span<const Function> functions);

}  // namespace vela::ir
