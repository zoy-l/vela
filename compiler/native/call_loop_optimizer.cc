#include "compiler/native/call_loop_optimizer.h"

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
#include <llvm/Transforms/Utils/LoopUtils.h>
#include <llvm/Transforms/Utils/UnrollLoop.h>

namespace vela::compiler::native {
namespace {

constexpr unsigned kCallLoopUnrollCount = 4;
// 余数路径至多再复制 count - 1 次；预算覆盖主循环和余数，而非只计算主循环。
constexpr unsigned kMaximumLoopCopies = (2 * kCallLoopUnrollCount) - 1;
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
    for (auto* loop : loops.getLoopsInPreorder()) {
      if (!loop->getSubLoops().empty() ||
          llvm::getUnrollMetadataForLoop(loop, "llvm.loop.unroll.disable") != nullptr ||
          llvm::getUnrollMetadataForLoop(loop, "llvm.loop.unroll.count") != nullptr ||
          llvm::getUnrollMetadataForLoop(loop, "llvm.loop.unroll.full") != nullptr ||
          !HasInvariantIndirectCall(*loop) ||
          evolution.getSmallConstantMaxTripCount(loop) < kCallLoopUnrollCount) {
        continue;
      }
      std::size_t instructions = 0;
      for (const auto* block : loop->blocks()) {
        instructions += block->size();
      }
      if (instructions * kMaximumLoopCopies <= kMaximumExpandedInstructions) {
        // 通用优化完成后再展开，保留每次调用、错误出口及观察边界，
        // 只摊薄间接调用循环的计数与回跳；不同时复制嵌套遍历。
        llvm::addStringMetadataToLoop(loop, "llvm.loop.unroll.count", kCallLoopUnrollCount);
      }
    }
    return llvm::PreservedAnalyses::all();
  }
};

}  // namespace

void AddCallLoopPasses(llvm::FunctionPassManager& passes) {
  passes.addPass(CallLoopHintsPass());
  passes.addPass(llvm::LoopUnrollPass(llvm::LoopUnrollOptions(3, true)));
}

}  // namespace vela::compiler::native
