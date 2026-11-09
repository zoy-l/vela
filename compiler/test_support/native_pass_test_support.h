#pragma once

#include <llvm/IR/PassManager.h>

namespace vela::compiler::native::test {

void RunFunctionPasses(llvm::Function& function, void (*add_passes)(llvm::FunctionPassManager&));

}  // namespace vela::compiler::native::test
