#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "compiler/ir/ir.h"

namespace vela::ir {

// origins 与输入指令一一对应；优化期间同步压缩，保持下游错误位置映射到规范 IR。
void Optimize(Function& function,
              std::span<const Function> functions,
              std::vector<std::size_t>* origins = nullptr);

}  // namespace vela::ir
