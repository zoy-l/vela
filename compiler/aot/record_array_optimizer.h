#pragma once

#include "compiler/ir/ir.h"

namespace vela::compiler::aot {

// 私有全局数组的元素只有构造、单次移交和字段访问时，身份可由稳定逻辑索引替代。
void PackPrivateRecordArrays(ir::Program& program);

}  // namespace vela::compiler::aot
