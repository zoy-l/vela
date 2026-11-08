#pragma once

namespace llvm {
class DominatorTree;
class Function;
}  // namespace llvm

namespace vela::compiler::native {

void PromoteLocalAllocations(llvm::Function& function, llvm::DominatorTree& dominators);

}  // namespace vela::compiler::native
