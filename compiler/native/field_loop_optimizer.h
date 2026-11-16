#pragma once

#include <llvm/IR/PassManager.h>

namespace vela::compiler::native {

void AddFieldLoopPasses(llvm::FunctionPassManager& passes);
void AddLateFieldGuardPasses(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::native
