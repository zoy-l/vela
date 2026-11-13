#pragma once

#include <llvm/IR/PassManager.h>

namespace vela::compiler::native {

// 将可交换的整数组重复更新合并为每个对象上的顺序更新；不重排同一字段的浮点加法。
void AddRepeatedFieldPasses(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::native
