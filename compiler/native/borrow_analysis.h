#pragma once

#include <optional>
#include <vector>

#include "compiler/ir/ir.h"
#include "compiler/native/escape_analysis.h"

namespace vela::compiler::native {

struct BorrowedFunction final {
  ir::ClassId type = 0;
  EscapePlan plan;
};

// 封闭 AOT 专用入口；首参数是字段地址表，不能按对象 ABI 调用。
std::vector<std::optional<BorrowedFunction>> PlanBorrows(const ir::Program& program);

}  // namespace vela::compiler::native
