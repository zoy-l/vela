#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "base/layout_description.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

[[nodiscard]] std::string HostIdentifier(std::string_view name);
[[nodiscard]] std::expected<std::string, std::string>
HostLogicalType(const layout::Program& program, layout::Type type, std::string_view name_space);
[[nodiscard]] std::expected<std::string, std::string> EmitHostDeclarations(
    const ir::Program& program,
    std::string_view name_space);

}  // namespace vela::compiler::native
