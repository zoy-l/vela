#pragma once

#include <optional>
#include <span>

#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"

namespace vela::compiler {

[[nodiscard]] auto AllocateRegisters(ir::Function& function,
                                     std::span<const ir::Function> functions)
    -> std::optional<frontend::Diagnostic>;

}  // namespace vela::compiler
