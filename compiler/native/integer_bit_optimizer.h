#pragma once

#include <llvm/IR/PassManager.h>

namespace vela::compiler::native {

void AddIntegerBitPasses(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::native
