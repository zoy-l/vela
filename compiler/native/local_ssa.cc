#include "compiler/native/local_ssa.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

namespace vela::compiler::native {

void PromoteLocalAllocations(llvm::Function& function, llvm::DominatorTree& dominators) {
  llvm::SmallVector<llvm::AllocaInst*> locals;
  for (auto& instruction : function.getEntryBlock()) {
    auto* local = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
    if (local != nullptr && llvm::isAllocaPromotable(local)) {
      locals.push_back(local);
    }
  }
  llvm::PromoteMemToReg(locals, dominators);
}

}  // namespace vela::compiler::native
