#pragma once

#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

// 对静态泛型用法生成共享的特化 AST；后续类型检查和后端只消费具体声明。
[[nodiscard]] auto SpecializeGenerics(Program& program) -> Diagnostics;

}  // namespace vela::frontend
