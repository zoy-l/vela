#pragma once

#include <llvm/IR/PassManager.h>

namespace vela::compiler::aot {

inline constexpr char kNativeRootClearMetadata[] = "vela.root.clear";

void AddIntegerLoopPasses(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::aot
