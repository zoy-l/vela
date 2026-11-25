#pragma once

#include <algorithm>
#include <optional>
#include <span>

#include <cstdint>
#include <string_view>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/PassManager.h>

namespace llvm {
class BasicBlock;
class CondBrInst;
class Function;
class Instruction;
}

namespace vela::compiler::native {

struct GeneratedFunction;
enum class FieldLoopUnrolling : std::uint8_t { Disabled, Enabled };

inline constexpr std::string_view kNativeRootClearMetadata = "vela.root.clear";
inline constexpr std::string_view kNativeBoundedPollMetadata = "vela.poll.bounded";
inline constexpr std::string_view kNativePollReadyStateMetadata = "vela.poll.ready.state";
// 标记完整慢路中的操作或入口分支；有界克隆只沿其外侧的守卫成功边执行。
inline constexpr std::string_view kNativeGuardFailureMetadata = "vela.guard.failure";
inline constexpr std::uint64_t kNativeMaximumUnpolledIterations = 1024;
inline constexpr std::uint64_t kNativeMaximumUnpolledInstructions = 131072;

constexpr std::uint64_t NativePollBudget(std::uint64_t instructions) {
  return std::min(kNativeMaximumUnpolledIterations,
                  kNativeMaximumUnpolledInstructions / instructions);
}

void MarkGuardFailure(llvm::Instruction& instruction);
void FindGuardFailureBlocks(const llvm::Function& function,
                            llvm::SmallPtrSetImpl<const llvm::BasicBlock*>& failures);
[[nodiscard]] std::optional<unsigned> FindReadyPollEdge(const llvm::CondBrInst& branch);

void AddIntegerLoopPasses(llvm::FunctionPassManager& passes, FieldLoopUnrolling fields);
void AddBoundedPollPasses(llvm::FunctionPassManager& passes, FieldLoopUnrolling fields);
void AddLateBoundedPollPasses(llvm::FunctionPassManager& passes);
void EmitBoundedLeafGuard(const GeneratedFunction& function,
                          std::span<const unsigned> array_parameters = {});

}  // namespace vela::compiler::native
