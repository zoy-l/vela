#pragma once

#include <cstdint>
#include <vector>

#include "compiler/ir/ir.h"

namespace vela::compiler::native {

// 规范 IR 中非逃逸 int[] 的全部写入共同约束读值。每个寄存器对应一个可能置位的位掩码；
// 全一表示未知，没有本地整数数组时返回空表。包含参数、未知定义或外部访问的别名组不提供事实。
[[nodiscard]] std::vector<std::uint64_t> FindPrivateArrayElementBits(const ir::Function& function,
                                                                     const ir::Program& program);

}  // namespace vela::compiler::native
