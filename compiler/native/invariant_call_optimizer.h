#pragma once

#include <string_view>

#include <llvm/IR/PassManager.h>

namespace llvm {
class BranchInst;
}

namespace vela::compiler::native {

// 标记可版本化的调用守卫；pass 仍须证明条件不变，或 Ready PHI 的所有回边均为 true。
inline constexpr std::string_view kNativeInvariantCallMetadata = "vela.call.invariant";

void MarkInvariantCall(llvm::BranchInst& branch);

void AddInvariantCallPasses(llvm::FunctionPassManager& passes);

}  // namespace vela::compiler::native
