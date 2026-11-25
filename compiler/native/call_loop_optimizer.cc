#include "compiler/native/call_loop_optimizer.h"

#include <algorithm>
#include <bit>
#include <cstddef>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Scalar/LoopUnrollPass.h>

#include "compiler/native/array_reduction_optimizer.h"
#include "compiler/native/loop_hints.h"

namespace vela::compiler::native {
namespace {

constexpr unsigned kMaximumCallLoopUnrollCount = 8;
constexpr unsigned kMinimumCallLoopUnrollCount = 4;
constexpr std::size_t kMaximumExpandedInstructions = 1024;

bool HasInvariantIndirectCall(const llvm::Loop& loop) {
  for (const auto* block : loop.blocks()) {
    for (const auto& instruction : *block) {
      const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call != nullptr && call->isIndirectCall() &&
          loop.isLoopInvariant(call->getCalledOperand())) {
        return true;
      }
    }
  }
  return false;
}

class CallLoopHintsPass : public llvm::PassInfoMixin<CallLoopHintsPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    const auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
    bool changed = false;
    for (auto* loop : loops.getLoopsInPreorder()) {
      const auto trips = evolution.getSmallConstantMaxTripCount(loop);
      if (!loop->getSubLoops().empty() || HasLoopUnrollHints(*loop) ||
          !HasInvariantIndirectCall(*loop) || (trips != 0 && trips < kMinimumCallLoopUnrollCount) ||
          (trips == 0 && !function.hasFnAttribute(kNativeArrayReductionCandidateAttribute))) {
        continue;
      }
      std::size_t instructions = 0;
      for (const auto* block : loop->blocks()) {
        instructions += block->size();
      }
      // 余数路径至多再复制 count - 1 次；预算同时覆盖主循环及余数。
      const auto budget = ((kMaximumExpandedInstructions / instructions) + 1) / 2;
      const auto maximum = std::min<std::size_t>(
          {kMaximumCallLoopUnrollCount, trips == 0 ? kMaximumCallLoopUnrollCount : trips, budget});
      const auto count = std::bit_floor(maximum);
      if (count >= kMinimumCallLoopUnrollCount) {
        // 通用优化完成后再展开，保留每次调用、错误出口及观察边界，
        // 只摊薄间接调用循环的计数与回跳；不同时复制嵌套遍历。
        SetLoopUnrollCount(*loop, static_cast<unsigned>(count), LoopUnrollHints::Retain);
        changed = true;
      }
    }
    return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
  }
};

}  // namespace

void AddCallLoopPasses(llvm::FunctionPassManager& passes) {
  passes.addPass(CallLoopHintsPass());
  passes.addPass(
      llvm::LoopUnrollPass(llvm::LoopUnrollOptions(3, true).setPartial(true).setRuntime(true)));
}

}  // namespace vela::compiler::native
