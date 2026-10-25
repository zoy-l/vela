#pragma once

#include <span>
#include <vector>

#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "runtime/object.h"

namespace vela::compiler {

[[nodiscard]] auto CreateTypeConstraints(
    const ir::Program& program,
    const frontend::SemanticModel& model,
    std::span<runtime::ClassLayout* const> classes,
    std::span<const runtime::ArrayLayout* const> arrays,
    std::span<const runtime::CollectionLayout* const> collections)
    -> std::vector<runtime::TypeConstraint>;

}  // namespace vela::compiler
