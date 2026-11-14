#include "compiler/native/recursive_inliner.h"

#include <algorithm>
#include <cstddef>
#include <span>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/LoopUtils.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "compiler/native/local_ssa.h"

namespace vela::compiler::native {
namespace {

llvm::SmallVector<llvm::CallBase*, 4> RecursiveCalls(llvm::Function& body,
                                                     const llvm::Function& target) {
  llvm::SmallVector<llvm::CallBase*, 4> calls;
  for (auto& instruction : llvm::instructions(body)) {
    auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (call != nullptr && call->getCalledFunction() == &target) {
      calls.push_back(call);
    }
  }
  return calls;
}

llvm::Function* CloneRecursiveBody(llvm::Function& function) {
  llvm::ValueToValueMapTy values;
  auto* clone = llvm::CloneFunction(&function, values);
  for (auto* call : RecursiveCalls(*clone, *clone)) {
    call->setCalledFunction(&function);
  }
  return clone;
}

void AdoptBody(llvm::Function& function, llvm::Function& candidate) {
  // 保留 Function 身份、链接属性和入口引用，只替换已完成计费的函数体。
  for (auto& argument : candidate.args()) {
    argument.replaceAllUsesWith(function.getArg(argument.getArgNo()));
  }
  for (auto& block : function) {
    block.dropAllReferences();
  }
  function.erase(function.begin(), function.end());
  function.splice(function.end(), &candidate);
}

bool HasRecursiveCall(const llvm::Loop& loop, std::span<llvm::CallBase* const> calls) {
  return std::ranges::any_of(calls, [&](const auto* call) { return loop.contains(call); });
}

bool HasBoundedLoopExpansion(const llvm::Loop& loop,
                             unsigned iterations,
                             const llvm::LoopInfo& loops,
                             llvm::ScalarEvolution& evolution) {
  constexpr std::size_t kMaximumExpandedInstructions = 4096;
  std::size_t instructions = 0;
  for (const auto* block : loop.blocks()) {
    std::size_t copies = iterations;
    for (const auto* nested = loops.getLoopFor(block); nested != &loop;
         nested = nested->getParentLoop()) {
      const auto trips = evolution.getSmallConstantMaxTripCount(nested);
      // 同时计入内层主循环和余数副本；无法界定的内层不接受外层强制展开。
      const auto maximum_copies = (2ULL * trips) - 1;
      if (trips == 0 || maximum_copies > kMaximumExpandedInstructions / copies) {
        return false;
      }
      copies *= maximum_copies;
    }
    if (block->size() > (kMaximumExpandedInstructions - instructions) / copies) {
      return false;
    }
    instructions += block->size() * copies;
  }
  return true;
}

void PreserveLoopCalls(llvm::Loop& loop) {
  for (auto* block : loop.blocks()) {
    for (auto& instruction : *block) {
      auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      const auto* target = call == nullptr ? nullptr : call->getCalledFunction();
      if (target != nullptr && !target->isDeclaration()) {
        // 强制内联的叶片段已在准备阶段展开；其余完整调用保留独立物理帧。
        // 这也避免 LLVM 因等待后续内联而跳过已显式请求的循环展开。
        call->addFnAttr(llvm::Attribute::NoInline);
      }
    }
  }
}

class RecursiveLoopHintsPass : public llvm::PassInfoMixin<RecursiveLoopHintsPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    constexpr std::size_t kMaximumIterations = 8;
    const auto calls = RecursiveCalls(function, function);
    if (calls.empty()) {
      return llvm::PreservedAnalyses::all();
    }
    auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
    for (auto* loop : loops.getLoopsInPreorder()) {
      if (!HasRecursiveCall(*loop, calls) ||
          std::ranges::any_of(loop->getSubLoops(),
                              [&](const auto* child) { return HasRecursiveCall(*child, calls); })) {
        continue;
      }
      auto iterations = evolution.getSmallConstantMaxTripCount(loop);
      // 前测循环还计入最后一次退出条件；只用其成功迭代上界选择展开因子，
      // 不据此删除条件或错误出口，具体复制仍交给 LLVM 的循环展开。
      if (iterations != 0 && loop->getHeader() != loop->getLoopLatch() &&
          loop->isLoopExiting(loop->getHeader()) &&
          evolution.getSmallConstantTripCount(loop, loop->getHeader()) == iterations) {
        --iterations;
      }
      if (iterations < 2 || iterations > kMaximumIterations) {
        continue;
      }
      if (!HasBoundedLoopExpansion(*loop, iterations, loops, evolution)) {
        continue;
      }
      // 内层保留 LLVM 自身的展开选择；预算已经乘入其副本，不能统一禁用而
      // 让短扫描在每个递归分支中重复支付计数和回跳成本。
      PreserveLoopCalls(*loop);
      llvm::addStringMetadataToLoop(loop, "llvm.loop.unroll.count", iterations);
    }
    return llvm::PreservedAnalyses::all();
  }
};

}  // namespace

void InlineRecursion(llvm::Function& function) {
  constexpr std::size_t kMaximumDepth = 3;
  llvm::DominatorTree dominators(function);
  PromoteLocalAllocations(function, dominators);
  const auto original_calls = RecursiveCalls(function, function);
  if (original_calls.empty() ||
      function.getInstructionCount() >= kMaximumRecursiveInlineInstructions) {
    return;
  }
  const llvm::LoopInfo loops(dominators);
  // 循环内递归展开会叠加完整循环和帧的活跃状态；指令总量预算无法约束其
  // 寄存器压力，保留独立调用以免多层循环不变量在每次进入时全部 spill。
  for (const auto* call : original_calls) {
    if (loops.getLoopFor(call->getParent()) != nullptr) {
      return;
    }
  }
  // 各层始终内联原始快照，避免把已经展开的函数再次复制进同一层。
  auto* snapshot = CloneRecursiveBody(function);
  for (std::size_t depth = 0; depth < kMaximumDepth; ++depth) {
    auto* candidate = CloneRecursiveBody(function);
    const auto calls = RecursiveCalls(*candidate, function);
    bool accepted = !calls.empty();
    for (auto* call : calls) {
      call->setCalledFunction(snapshot);
      llvm::InlineFunctionInfo information;
      if (!llvm::InlineFunction(*call, information).isSuccess() ||
          candidate->getInstructionCount() > kMaximumRecursiveInlineInstructions) {
        accepted = false;
        break;
      }
    }
    if (accepted) {
      AdoptBody(function, *candidate);
    }
    candidate->eraseFromParent();
    if (!accepted) {
      break;
    }
  }
  snapshot->eraseFromParent();
}

void AddRecursiveLoopHints(llvm::FunctionPassManager& passes) {
  passes.addPass(RecursiveLoopHintsPass());
}

}  // namespace vela::compiler::native
