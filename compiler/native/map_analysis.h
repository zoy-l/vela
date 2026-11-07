#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "compiler/ir/ir.h"

namespace vela::compiler::native {

// 返回每个 Map.set 对应的支配性 Map.get；两者之间的路径不调用用户代码、
// 不读写其他可变状态，且 map / key 都来自相同的单定义 Move 链。
std::vector<std::optional<std::size_t>> FindMapLookups(const ir::Function& function,
                                                       std::span<const ir::Function> functions);

}  // namespace vela::compiler::native
