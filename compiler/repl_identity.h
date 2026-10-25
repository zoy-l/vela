#pragma once

#include "compiler/frontend/ast.h"

namespace vela::compiler {

// 泛型特化后以词法所有者和源码坐标标识 lambda，不依赖语义检查遍历顺序。
void NameSessionLambdas(frontend::Program& program);

}  // namespace vela::compiler
