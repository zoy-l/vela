#include "compiler/native/invariant_call_optimizer.h"

#include <cstddef>
#include <utility>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/LCSSA.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "compiler/native/loop_cloning.h"

namespace vela::compiler::native {
namespace {

// 优先复制受预算约束的外层循环，不符合契约时检查子循环；同一区域只版本化一次。
constexpr std::size_t kMaximumVersionedLoopInstructions = 1024;

struct InvariantCall final {
  llvm::BranchInst* branch;
  llvm::Value* entry_condition;
};

struct InvariantCallPlan final {
  llvm::Loop* loop;
  llvm::SmallVector<InvariantCall> calls;
};

llvm::Value* InvariantEntryCondition(llvm::Loop& loop,
                                     llvm::Value& condition,
                                     llvm::BasicBlock& preheader) {
  if (loop.isLoopInvariant(&condition)) {
    return &condition;
  }
  auto* phi = llvm::dyn_cast<llvm::PHINode>(&condition);
  if (phi == nullptr || phi->getParent() != loop.getHeader()) {
    return nullptr;
  }
  // 全局 Ready 可在首轮初始化后变为 true。所有回边均为 true 时，入口状态
  // 才能证明整个克隆；慢路径仍须保留初始化后的状态更新。
  for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
    if (phi->getIncomingBlock(index) == &preheader) {
      continue;
    }
    const auto* ready = llvm::dyn_cast<llvm::ConstantInt>(phi->getIncomingValue(index));
    if (ready == nullptr || !ready->isOne()) {
      return nullptr;
    }
  }
  return phi->getIncomingValueForBlock(&preheader);
}

llvm::SmallVector<InvariantCall> FindInvariantCalls(llvm::Loop& loop,
                                                    llvm::DominatorTree& dominators) {
  auto* preheader = loop.getLoopPreheader();
  if (preheader == nullptr) {
    return {};
  }
  std::size_t instructions = 0;
  llvm::SmallVector<InvariantCall> selected;
  for (auto* block : loop.blocks()) {
    instructions += block->size();
    if (instructions > kMaximumVersionedLoopInstructions) {
      return {};
    }
    auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (branch == nullptr || !branch->isConditional() ||
        branch->getMetadata(kNativeInvariantCallMetadata) == nullptr) {
      continue;
    }
    auto* condition = branch->getCondition();
    if (llvm::isa<llvm::ConstantInt>(condition)) {
      continue;
    }
    auto* entry_condition = InvariantEntryCondition(loop, *condition, *preheader);
    if (entry_condition == nullptr) {
      continue;
    }
    const auto* definition = llvm::dyn_cast<llvm::Instruction>(entry_condition);
    if (definition == nullptr || dominators.dominates(definition, preheader->getTerminator())) {
      selected.push_back({.branch = branch, .entry_condition = entry_condition});
    }
  }
  return selected;
}

void VersionInvariantCalls(const InvariantCallPlan& plan) {
  auto& loop = *plan.loop;
  auto* terminator = loop.getLoopPreheader()->getTerminator();
  // 原分支可能只在非空循环中执行；freeze 避免入口分流扩大 poison 的可观察范围。
  llvm::IRBuilder<> guard(terminator);
  llvm::Value* ready = guard.getTrue();
  llvm::SmallPtrSet<llvm::Value*, 4> conditions;
  for (const auto& selected : plan.calls) {
    if (conditions.insert(selected.entry_condition).second) {
      ready =
          guard.CreateAnd(ready, guard.CreateFreeze(selected.entry_condition, "intrinsic.ready"));
    }
  }
  llvm::ValueToValueMapTy mapped;
  VersionLoop(loop, *ready, mapped, ".intrinsic");
  for (const auto& selected : plan.calls) {
    auto* branch = selected.branch;
    auto* cloned = llvm::cast<llvm::BranchInst>(mapped.lookup(branch));
    branch->setMetadata(kNativeInvariantCallMetadata, nullptr);
    cloned->setMetadata(kNativeInvariantCallMetadata, nullptr);
    // 合取为 false 不说明其中某个条件为 false；慢路径保留各自守卫及 Ready 更新。
    if (conditions.size() == 1 && selected.entry_condition == branch->getCondition()) {
      branch->setCondition(guard.getFalse());
    }
    cloned->setCondition(guard.getTrue());
  }
}

void CollectInvariantCalls(llvm::Loop& loop,
                           llvm::DominatorTree& dominators,
                           llvm::SmallVectorImpl<InvariantCallPlan>& plans) {
  auto calls = FindInvariantCalls(loop, dominators);
  if (!calls.empty()) {
    plans.push_back({.loop = &loop, .calls = std::move(calls)});
    return;
  }
  for (auto* child : loop.getSubLoops()) {
    CollectInvariantCalls(*child, dominators, plans);
  }
}

class InvariantCallPass : public llvm::PassInfoMixin<InvariantCallPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    llvm::SmallVector<InvariantCallPlan> plans;
    for (auto* loop : loops) {
      CollectInvariantCalls(*loop, dominators, plans);
    }
    for (const auto& plan : plans) {
      VersionInvariantCalls(plan);
    }
    if (!plans.empty()) {
      llvm::removeUnreachableBlocks(function);
      return llvm::PreservedAnalyses::none();
    }
    return llvm::PreservedAnalyses::all();
  }
};

}  // namespace

void MarkInvariantCall(llvm::BranchInst& branch) {
  branch.setMetadata(kNativeInvariantCallMetadata, llvm::MDNode::get(branch.getContext(), {}));
}

void AddInvariantCallPasses(llvm::FunctionPassManager& passes) {
  passes.addPass(llvm::EarlyCSEPass());
  passes.addPass(llvm::InstCombinePass());
  passes.addPass(llvm::SimplifyCFGPass());
  passes.addPass(llvm::LoopSimplifyPass());
  passes.addPass(llvm::LCSSAPass());
  passes.addPass(InvariantCallPass());
  passes.addPass(llvm::SimplifyCFGPass());
  passes.addPass(llvm::DCEPass());
}

}  // namespace vela::compiler::native
