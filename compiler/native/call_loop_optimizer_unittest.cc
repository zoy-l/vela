#include "compiler/native/call_loop_optimizer.h"

#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Utils/LoopUtils.h>

#include "compiler/native/loop_hints.h"
#include "compiler/test_support/native_pass_test_support.h"

namespace vela::compiler::native {
namespace {

std::size_t IndirectCallCount(const llvm::Function& function) {
  std::size_t count = 0;
  for (const auto& instruction : llvm::instructions(function)) {
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (call != nullptr && call->isIndirectCall()) {
      ++count;
    }
  }
  return count;
}

void ExpectUnexpandedCalls(llvm::Module& module) {
  for (auto& function : module) {
    test::RunFunctionPasses(function, AddCallLoopPasses);
    EXPECT_EQ(IndirectCallCount(function), 1U) << function.getName().str();
  }
  EXPECT_FALSE(llvm::verifyModule(module));
}

TEST(LoopHintsTest, RecognizesAllUnrollPropertiesAndReplacesOnlyTheirNamespace) {
  constexpr unsigned kReplacementCount = 8;
  for (const auto* property : {
           "llvm.loop.unroll.disable",
           "llvm.loop.unroll.enable",
           "llvm.loop.unroll.full",
           "llvm.loop.unroll.count",
           "llvm.loop.unroll.partial",
           "llvm.loop.unroll.runtime.disable",
           "llvm.loop.unroll.followup_all",
       }) {
    SCOPED_TRACE(property);
    llvm::LLVMContext context;
    const auto module = test::ParseModule(R"(
define void @loop(i1 %again) {
entry:
  br label %body
body:
  br i1 %again, label %body, label %exit, !llvm.loop !0
exit:
  ret void
}
!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.vectorize.enable", i1 true}
)",
                                          context);
    ASSERT_NE(module, nullptr);
    test::WithFunctionLoops(*module, [&](auto&, const auto& loops) {
      auto* loop = *loops.begin();
      EXPECT_FALSE(HasLoopUnrollHints(*loop));
      llvm::addStringMetadataToLoop(loop, property, 2);
      EXPECT_TRUE(HasLoopUnrollHints(*loop));
      SetLoopUnrollCount(*loop, 4, LoopUnrollHints::Retain);
      EXPECT_NE(llvm::findOptionMDForLoop(loop, property), nullptr);
      SetLoopUnrollCount(*loop, kReplacementCount, LoopUnrollHints::Replace);
      EXPECT_EQ(loop->getLoopID()->getNumOperands(), 3U);
      EXPECT_NE(llvm::findOptionMDForLoop(loop, "llvm.loop.vectorize.enable"), nullptr);
      EXPECT_NE(llvm::findOptionMDForLoop(loop, "llvm.loop.unroll.count"), nullptr);
      if (std::string_view(property) != "llvm.loop.unroll.count") {
        EXPECT_EQ(llvm::findOptionMDForLoop(loop, property), nullptr);
      }
    });
    EXPECT_FALSE(llvm::verifyModule(*module));
  }
}

TEST(CallLoopOptimizerTest, ExpandsConstantCountCallsAndKeepsFailureExits) {
  llvm::LLVMContext context;
  const auto module = test::ParseModule(R"(
define i64 @invoke(ptr %target) {
entry:
  br label %loop
loop:
  %index = phi i64 [ 0, %entry ], [ %next, %latch ]
  %result = call i64 %target(i64 %index)
  %failed = icmp slt i64 %result, 0
  br i1 %failed, label %failure, label %latch
latch:
  %next = add nuw nsw i64 %index, 1
  %more = icmp ult i64 %next, 64
  br i1 %more, label %loop, label %done
failure:
  ret i64 %index
done:
  ret i64 64
}
)",
                                        context);
  ASSERT_NE(module, nullptr);
  auto& function = *module->getFunction("invoke");
  test::RunFunctionPasses(function, AddCallLoopPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  EXPECT_EQ(IndirectCallCount(function), 4U);
}

TEST(CallLoopOptimizerTest, LeavesSmallUnknownAndVariantTargetsUnchanged) {
  llvm::LLVMContext context;
  const auto module = test::ParseModule(R"(
define void @small(ptr %target) {
entry:
  br label %loop
loop:
  %index = phi i64 [ 0, %entry ], [ %next, %loop ]
  call void %target()
  %next = add nuw nsw i64 %index, 1
  %more = icmp ult i64 %next, 3
  br i1 %more, label %loop, label %done
done:
  ret void
}
define void @unknown(ptr %target, i64 %count) {
entry:
  br label %loop
loop:
  %index = phi i64 [ 0, %entry ], [ %next, %loop ]
  call void %target()
  %next = add i64 %index, 1
  %more = icmp ult i64 %next, %count
  br i1 %more, label %loop, label %done
done:
  ret void
}
define void @variant(ptr %slot) {
entry:
  br label %loop
loop:
  %index = phi i64 [ 0, %entry ], [ %next, %loop ]
  %target = load ptr, ptr %slot
  call void %target()
  %next = add nuw nsw i64 %index, 1
  %more = icmp ult i64 %next, 64
  br i1 %more, label %loop, label %done
done:
  ret void
}
)",
                                        context);
  ASSERT_NE(module, nullptr);
  ExpectUnexpandedCalls(*module);
}

TEST(CallLoopOptimizerTest, RespectsExpandedInstructionBudgetAndDisabledLoops) {
  std::string source = R"(
define void @large(ptr %target, ptr %slot) {
entry:
  br label %loop
loop:
  %index = phi i64 [ 0, %entry ], [ %next, %loop ]
  call void %target()
)";
  constexpr std::size_t kPaddingInstructions = 256;
  for (std::size_t index = 0; index < kPaddingInstructions; ++index) {
    source += "  %padding" + std::to_string(index) + " = load volatile i64, ptr %slot\n";
  }
  source += R"(
  %next = add nuw nsw i64 %index, 1
  %more = icmp ult i64 %next, 64
  br i1 %more, label %loop, label %done
done:
  ret void
}
define void @disabled(ptr %target) {
entry:
  br label %loop
loop:
  %index = phi i64 [ 0, %entry ], [ %next, %loop ]
  call void %target()
  %next = add nuw nsw i64 %index, 1
  %more = icmp ult i64 %next, 64
  br i1 %more, label %loop, label %done, !llvm.loop !0
done:
  ret void
}
!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.unroll.disable"}
)";
  llvm::LLVMContext context;
  const auto module = test::ParseModule(source, context);
  ASSERT_NE(module, nullptr);
  ExpectUnexpandedCalls(*module);
}

}  // namespace
}  // namespace vela::compiler::native
