#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

// 空局部数组的 reserve 支配计数循环，且每轮至多一次 push、没有别名逃逸。
[[nodiscard]] std::vector<std::uint8_t> FindReservedArrayPushes(
    const ir::Function& function,
    std::span<const ir::RegisterAccess> accesses);

}  // namespace vela::compiler::native
