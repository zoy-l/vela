#pragma once

#include <cstdint>
#include <vector>

#include "compiler/ir/ir.h"

namespace vela::compiler::native {

// 在规范 IR 上合并非逃逸 int[] 的全部写入；封闭程序还合并经全局槽共享的别名。
// 按函数、寄存器返回可能置位的位掩码，全一表示未知。参数、未知定义和外部访问使整个
// 别名组失效；可重载程序只证明函数内数组，不能假设其他代码版本的全局写入受约束。
[[nodiscard]] std::vector<std::vector<std::uint64_t>> FindArrayElementBits(
    const ir::Program& program);

}  // namespace vela::compiler::native
