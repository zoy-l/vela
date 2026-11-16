#pragma once

#include <string_view>

#include <llvm/IR/PassManager.h>

namespace vela::compiler::native {

inline constexpr std::string_view kNativeArrayReductionCandidateAttribute =
    "vela.array.reduction.candidate";
inline constexpr std::string_view kNativeArrayReductionFallbackMetadata =
    "vela.array.reduction.fallback";

void AddArrayReductionPasses(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::native
