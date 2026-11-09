#include "compiler/native/recursive_inliner.h"

#include <cstddef>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Utils/Cloning.h>
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

}  // namespace vela::compiler::native
