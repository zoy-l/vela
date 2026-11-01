#pragma once

#include <llvm/IR/PassManager.h>
#include <string_view>

namespace vela::compiler::native {

struct GeneratedFunction;

inline constexpr std::string_view kNativeRootClearMetadata = "vela.root.clear";
inline constexpr std::string_view kNativeBoundedPollMetadata = "vela.poll.bounded";

void AddIntegerLoopPasses(llvm::FunctionPassManager& passes);
void EmitBoundedLeafGuard(const GeneratedFunction& function);

}  // namespace vela::compiler::native
