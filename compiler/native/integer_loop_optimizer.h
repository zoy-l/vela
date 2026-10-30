#pragma once

#include <llvm/IR/PassManager.h>
#include <string_view>

namespace vela::compiler::native {

inline constexpr std::string_view kNativeRootClearMetadata = "vela.root.clear";

void AddIntegerLoopPasses(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::native
