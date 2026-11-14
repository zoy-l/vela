#pragma once

#include <cstdint>
#include <string_view>

#include <llvm/IR/PassManager.h>

namespace vela::compiler::native {

struct GeneratedFunction;

inline constexpr std::string_view kNativeRootClearMetadata = "vela.root.clear";
inline constexpr std::string_view kNativeBoundedPollMetadata = "vela.poll.bounded";
inline constexpr std::string_view kNativePollReadyStateMetadata = "vela.poll.ready.state";
inline constexpr std::string_view kNativeGuardFailureMetadata = "vela.guard.failure";
inline constexpr std::uint64_t kNativeMaximumUnpolledIterations = 1024;
inline constexpr std::uint64_t kNativeMaximumUnpolledInstructions = 131072;

void AddIntegerLoopPasses(llvm::FunctionPassManager& passes);
void AddBoundedPollPasses(llvm::FunctionPassManager& passes);
void AddLateBoundedPollPasses(llvm::FunctionPassManager& passes);
void EmitBoundedLeafGuard(const GeneratedFunction& function);

}  // namespace vela::compiler::native
