#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "compiler/ir/ir.h"

namespace vela::compiler::aot {

// 裁剪函数与成员并同步所有函数编号；类型编号保持稳定以沿用 Typed IR 的布局事实。
void PruneNativeProgram(ir::Program& program,
                        std::span<const std::string> exports,
                        std::optional<std::uint32_t>& main);

}  // namespace vela::compiler::aot
