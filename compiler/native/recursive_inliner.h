#pragma once

#include <cstddef>

#include <llvm/IR/PassManager.h>

namespace llvm {
class Function;
}

namespace vela::compiler::native {

inline constexpr std::size_t kMaximumRecursiveInlineInstructions = 8192;

// 只展开循环外的递归调用；预算包含帧维护和恢复代码的 LLVM 指令，
// 每次只发布完整且未超限的展开层。
void InlineRecursion(llvm::Function& function);

// 有界短循环只展开迭代，不展开递归帧；内部遍历保持紧凑，调用与失败边界不变。
void AddRecursiveLoopHints(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::native
