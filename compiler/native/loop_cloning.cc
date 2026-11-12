#include "compiler/native/loop_cloning.h"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

namespace vela::compiler::native {

void CloneLoopBody(llvm::Loop& loop, llvm::ValueToValueMapTy& mapped, llvm::StringRef suffix) {
  auto* function = loop.getHeader()->getParent();
  llvm::SmallVector<llvm::BasicBlock*> clones;
  for (auto* block : loop.blocks()) {
    auto* cloned = llvm::CloneBasicBlock(block, mapped, suffix, function);
    // CloneBasicBlock 只登记指令映射；控制流重映射还需要调用者登记基本块。
    mapped.insert({block, cloned});
    clones.push_back(cloned);
  }
  llvm::remapInstructionsInBlocks(clones, mapped);
  for (const auto& [block, cloned] : llvm::zip(loop.blocks(), clones)) {
    for (auto* successor : llvm::successors(block)) {
      if (loop.contains(successor)) {
        continue;
      }
      for (auto& phi : successor->phis()) {
        auto* incoming = phi.getIncomingValueForBlock(block);
        const auto found = mapped.find(incoming);
        // 循环外的 SSA 值仍由原定义支配克隆；未映射的出口输入保留原值。
        auto* cloned_incoming =
            found == mapped.end() ? incoming : static_cast<llvm::Value*>(found->second);
        // switch 可多次指向同一出口；LLVM 的 PHI 按前驱边计数，不能按出口块去重。
        phi.addIncoming(cloned_incoming, cloned);
      }
    }
  }
}

void VersionLoop(llvm::Loop& loop,
                 llvm::Value& condition,
                 llvm::ValueToValueMapTy& mapped,
                 llvm::StringRef suffix) {
  CloneLoopBody(loop, mapped, suffix);
  auto* header = loop.getHeader();
  auto* terminator = loop.getLoopPreheader()->getTerminator();
  auto* fast = llvm::cast<llvm::BasicBlock>(mapped.lookup(header));
  llvm::CondBrInst::Create(&condition, fast, header, terminator->getIterator());
  terminator->eraseFromParent();
}

}  // namespace vela::compiler::native
