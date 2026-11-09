#pragma once

#include <cstddef>

namespace llvm {
class Function;
}

namespace vela::compiler::native {

inline constexpr std::size_t kMaximumRecursiveInlineInstructions = 8192;

// 只展开循环外的递归调用；预算包含帧维护和恢复代码的 LLVM 指令，
// 每次只发布完整且未超限的展开层。
void InlineRecursion(llvm::Function& function);

}  // namespace vela::compiler::native
