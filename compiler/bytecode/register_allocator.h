#pragma once

#include <optional>
#include <span>

#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"

namespace vela::compiler {

[[nodiscard]] std::optional<frontend::Diagnostic> AllocateRegisters(
    ir::Function& function,
    std::span<const ir::Function> functions);

}  // namespace vela::compiler
