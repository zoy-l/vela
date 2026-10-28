#pragma once

#include "compiler/ir/ir.h"

namespace vela::compiler::aot {

// AOT async/import 生产者已绑定其结果类型；移除同一基本块内的重复绑定。
void ElideNativeResultBindings(ir::Program& program);

}  // namespace vela::compiler::aot
