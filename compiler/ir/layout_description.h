#pragma once

#include <cstdint>

#include "base/layout_description.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

[[nodiscard]] std::uint32_t FieldSlotCount(const frontend::TypeDescriptor& type);
[[nodiscard]] std::uint32_t PhysicalFieldOffset(const Class& owner, std::uint32_t field);

[[nodiscard]] layout::Program DescribeLayouts(const Program& program,
                                              const frontend::SemanticModel& model,
                                              const frontend::Program& source);

}  // namespace vela::ir
