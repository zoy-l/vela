#include "compiler/native/loop_cloning.h"

#include <gtest/gtest.h>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

namespace vela::compiler::native {
namespace {

TEST(LoopCloningTest, MapsBlocksAndInstructionsAndRemapsEntryBackedgesAndExitValues) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  const auto module = llvm::parseAssemblyString(R"(
define i64 @calculate(i1 %ready, i64 %count) {
entry:
  br label %header
header:
  %i = phi i64 [ 0, %entry ], [ %next, %header ]
  %next = add i64 %i, 1
  %more = icmp slt i64 %next, %count
  br i1 %more, label %header, label %done
done:
  %result = phi i64 [ %next, %header ]
  ret i64 %result
}
)",
                                                diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  auto& function = *module->getFunction("calculate");
  llvm::DominatorTree dominators(function);
  llvm::LoopInfo loops(dominators);
  ASSERT_FALSE(loops.empty());
  auto& loop = **loops.begin();
  auto* header = loop.getHeader();
  llvm::ValueToValueMapTy mapped;
  VersionLoop(loop, *function.getArg(0), mapped, ".cloned");
  auto* cloned = llvm::dyn_cast_or_null<llvm::BasicBlock>(mapped.lookup(header));
  ASSERT_NE(cloned, nullptr);
  EXPECT_NE(cloned, header);
  EXPECT_EQ(cloned->getParent(), &function);
  for (auto& instruction : *header) {
    const auto* mapped_instruction =
        llvm::dyn_cast_or_null<llvm::Instruction>(mapped.lookup(&instruction));
    ASSERT_NE(mapped_instruction, nullptr);
    EXPECT_EQ(mapped_instruction->getParent(), cloned);
  }
  const auto& dispatch = llvm::cast<llvm::BranchInst>(*function.getEntryBlock().getTerminator());
  EXPECT_EQ(dispatch.getCondition(), function.getArg(0));
  EXPECT_EQ(dispatch.getSuccessor(0), cloned);
  EXPECT_EQ(dispatch.getSuccessor(1), header);
  const auto& induction = llvm::cast<llvm::PHINode>(header->front());
  const auto& cloned_induction = llvm::cast<llvm::PHINode>(cloned->front());
  EXPECT_EQ(cloned_induction.getIncomingBlock(1), cloned);
  EXPECT_EQ(cloned_induction.getIncomingValue(1), mapped.lookup(induction.getIncomingValue(1)));
  const auto& backedge = llvm::cast<llvm::BranchInst>(*cloned->getTerminator());
  EXPECT_EQ(backedge.getSuccessor(0), cloned);
  const auto& result = llvm::cast<llvm::PHINode>(backedge.getSuccessor(1)->front());
  EXPECT_EQ(result.getIncomingValueForBlock(cloned), mapped.lookup(induction.getIncomingValue(1)));
  EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
}

TEST(LoopCloningTest, PreservesRepeatedExitEdgesInSuccessorPhis) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  const auto module = llvm::parseAssemblyString(R"(
define i64 @calculate(i1 %ready, i64 %selector, i64 %count) {
entry:
  br label %header
header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %more = icmp slt i64 %i, %count
  br i1 %more, label %body, label %done
body:
  switch i64 %selector, label %latch [ i64 1, label %done
                                     i64 3, label %done ]
latch:
  %next = add nsw i64 %i, 1
  br label %header
done:
  %result = phi i64 [ %i, %header ], [ %i, %body ], [ %i, %body ]
  ret i64 %result
}
)",
                                                diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  ASSERT_FALSE(llvm::verifyModule(*module));
  auto& function = *module->getFunction("calculate");
  llvm::DominatorTree dominators(function);
  llvm::LoopInfo loops(dominators);
  ASSERT_FALSE(loops.empty());
  auto& loop = **loops.begin();
  llvm::ValueToValueMapTy mapped;
  VersionLoop(loop, *function.getArg(0), mapped, ".cloned");
  EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
}

}  // namespace
}  // namespace vela::compiler::native
