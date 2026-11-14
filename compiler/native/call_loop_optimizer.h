#pragma once

#include <llvm/IR/PassManager.h>

namespace vela::compiler::native {

void AddCallLoopPasses(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::native
