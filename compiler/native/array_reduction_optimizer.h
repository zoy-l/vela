#pragma once

#include <string_view>

#include <llvm/IR/PassManager.h>

namespace vela::compiler::native {

inline constexpr std::string_view kNativeArrayReductionCandidateAttribute =
    "vela.array.reduction.candidate";
// LLVM 的循环元数据接口要求带 NUL 结尾的名称。
inline constexpr const char* kNativeArrayReductionFallbackMetadata =
    "vela.array.reduction.fallback";

void AddArrayReductionPasses(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::native
