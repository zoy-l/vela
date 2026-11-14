#pragma once

#include <string_view>

#include <llvm/IR/PassManager.h>

namespace vela::compiler::native {

inline constexpr std::string_view kNativeArrayReductionCandidateAttribute =
    "vela.array.reduction.candidate";

void AddArrayReductionPasses(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::native
