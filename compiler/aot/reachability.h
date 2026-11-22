#pragma once

#include <cstdint>
#include <optional>

#include "compiler/ir/ir.h"

namespace vela::compiler::aot {

// 裁剪不可调用函数和契约外实现类并同步其编号；运行时类型编号保持稳定。
void PruneNativeProgram(ir::Program& program, std::optional<std::uint32_t>& main);

}  // namespace vela::compiler::aot
