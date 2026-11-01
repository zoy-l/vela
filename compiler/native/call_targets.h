#pragma once

#include <optional>
#include <vector>

#include "base/callable.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

// 只证明本函数创建且未覆写的 callable；索引对应输入 IR，不改变恢复位置。
std::vector<std::optional<CallableTarget>> FindDirectCalls(const ir::Function& function,
                                                           const ir::Program& program);

}  // namespace vela::compiler::native
