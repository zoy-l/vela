#pragma once

#include <llvm/IR/PassManager.h>
#include <string_view>

namespace vela::compiler::native {

struct GeneratedFunction;

inline constexpr std::string_view kNativeRootClearMetadata = "vela.root.clear";
inline constexpr std::string_view kNativeBoundedPollMetadata = "vela.poll.bounded";
inline constexpr std::string_view kNativePollReadyStateMetadata = "vela.poll.ready.state";
inline constexpr std::string_view kNativeGuardFailureMetadata = "vela.guard.failure";

void AddIntegerLoopPasses(llvm::FunctionPassManager& passes);
void AddBoundedPollPasses(llvm::FunctionPassManager& passes);
void EmitBoundedLeafGuard(const GeneratedFunction& function);

}  // namespace vela::compiler::native
