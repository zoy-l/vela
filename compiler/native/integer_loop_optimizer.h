#pragma once

#include <llvm/IR/PassManager.h>

namespace vela::compiler::native {

inline constexpr char kNativeRootClearMetadata[] = "vela.root.clear";

void AddIntegerLoopPasses(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::native
