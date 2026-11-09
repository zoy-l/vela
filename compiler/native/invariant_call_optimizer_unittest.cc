#include "compiler/native/invariant_call_optimizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include <gtest/gtest.h>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "compiler/test_support/native_pass_test_support.h"

namespace vela::compiler::native {
namespace {

enum class BindingGuard : std::uint8_t { Invariant, Varying, Unmarked, OverBudget };

class InvariantCallOptimizerTest : public testing::TestWithParam<BindingGuard> {};

TEST_P(InvariantCallOptimizerTest, VersionsNestedLoopsWithLiveOutsOnlyWithinTheContractAndBudget) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  const auto module = llvm::parseAssemblyString(R"(
declare i64 @fast(i64)
declare i64 @host(i64)
declare void @tick()
define i64 @calculate(i1 %ready, i64 %count) {
entry:
  br label %outer
outer:
  %i = phi i64 [ 0, %entry ], [ %next.i, %outer.latch ]
  %total = phi i64 [ 0, %entry ], [ %subtotal, %outer.latch ]
  %more = icmp slt i64 %i, %count
  br i1 %more, label %inner.preheader, label %done
inner.preheader:
  br label %inner
inner:
  %j = phi i64 [ 0, %inner.preheader ], [ %next.j, %latch ]
  %sum = phi i64 [ %total, %inner.preheader ], [ %next.sum, %latch ]
  %inner.more = icmp slt i64 %j, 3
  br i1 %inner.more, label %binding, label %inner.done
binding:
  %local.ready = icmp eq i64 %j, 0
  br i1 %ready, label %intrinsic, label %bound
intrinsic:
  %direct = call i64 @fast(i64 %j)
  br label %joined
bound:
  %checked = call i64 @host(i64 %j)
  br label %joined
joined:
  %value = phi i64 [ %direct, %intrinsic ], [ %checked, %bound ]
  %bad = icmp eq i64 %value, -1
  br i1 %bad, label %failed, label %latch
latch:
  %next.sum = add i64 %sum, %value
  %next.j = add i64 %j, 1
  br label %inner
inner.done:
  %subtotal = phi i64 [ %sum, %inner ]
  br label %outer.latch
outer.latch:
  %next.i = add i64 %i, 1
  br label %outer
done:
  %result = phi i64 [ %total, %outer ]
  ret i64 %result
failed:
  %failure = phi i64 [ %value, %joined ]
  ret i64 %failure
}
)",
                                                diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  auto& function = *module->getFunction("calculate");
  const auto block =
      std::ranges::find_if(function, [](const auto& item) { return item.getName() == "binding"; });
  ASSERT_NE(block, function.end());
  auto& branch = llvm::cast<llvm::BranchInst>(*block->getTerminator());
  const auto scenario = GetParam();
  if (scenario != BindingGuard::Unmarked) {
    MarkInvariantCall(branch);
  }
  if (scenario == BindingGuard::Varying) {
    branch.setCondition(&block->front());
  }
  if (scenario == BindingGuard::OverBudget) {
    constexpr std::size_t kOverBudgetCalls = 1030;
    llvm::IRBuilder<> builder(&branch);
    for (std::size_t index = 0; index < kOverBudgetCalls; ++index) {
      builder.CreateCall(module->getFunction("tick"));
    }
  }
  ASSERT_FALSE(llvm::verifyModule(*module));
  test::RunFunctionPasses(function, AddInvariantCallPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  const auto count_versions = [&function] {
    return std::ranges::count_if(
        function, [](const auto& item) { return item.getName().ends_with(".intrinsic"); });
  };
  const auto versions = count_versions();
  EXPECT_EQ(versions > 0, scenario == BindingGuard::Invariant);
  if (scenario == BindingGuard::Invariant) {
    for (const auto& item : function) {
      EXPECT_EQ(item.getTerminator()->getMetadata(kNativeInvariantCallMetadata), nullptr);
    }
  }
  // 生产流水线会在多个阶段调用此 pass；同一个绑定不能反复复制循环。
  test::RunFunctionPasses(function, AddInvariantCallPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  EXPECT_EQ(count_versions(), versions);
}

INSTANTIATE_TEST_SUITE_P(GuardContracts,
                         InvariantCallOptimizerTest,
                         testing::Values(BindingGuard::Invariant,
                                         BindingGuard::Varying,
                                         BindingGuard::Unmarked,
                                         BindingGuard::OverBudget));

class InitializedCallOptimizerTest : public testing::TestWithParam<bool> {};

TEST_P(InitializedCallOptimizerTest, VersionsReadyEntryWithoutFreezingTheUninitializedLoop) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  const auto module = llvm::parseAssemblyString(R"(
declare void @initialize()
define i64 @calculate(i1 %ready, i1 %next.ready, i64 %count) {
entry:
  br label %header
header:
  %i = phi i64 [ 0, %entry ], [ %next.i, %latch ]
  %established = phi i1 [ %ready, %entry ], [ true, %latch ]
  %more = icmp slt i64 %i, %count
  br i1 %more, label %binding, label %done
binding:
  br i1 %established, label %latch, label %initialize, !vela.call.invariant !0
initialize:
  call void @initialize()
  br label %latch
latch:
  %next.i = add nsw i64 %i, 1
  br label %header
done:
  ret i64 %i
}
!0 = !{}
)",
                                                diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  auto& function = *module->getFunction("calculate");
  const auto monotonic = GetParam();
  if (!monotonic) {
    for (auto& block : function) {
      for (auto& phi : block.phis()) {
        if (phi.getName() == "established") {
          phi.setIncomingValue(1, function.getArg(1));
        }
      }
    }
  }
  ASSERT_FALSE(llvm::verifyModule(*module));
  test::RunFunctionPasses(function, AddInvariantCallPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  std::size_t initialized_versions = 0;
  std::size_t initializer_calls = 0;
  std::size_t changing_guards = 0;
  for (auto& block : function) {
    initialized_versions += block.getName().ends_with(".intrinsic") ? 1 : 0;
    if (const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
        branch != nullptr && branch->isConditional() &&
        llvm::isa<llvm::PHINode>(branch->getCondition())) {
      ++changing_guards;
    }
    for (const auto& instruction : block) {
      if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
          call != nullptr && call->getCalledFunction()->getName() == "initialize") {
        ++initializer_calls;
      }
    }
  }
  EXPECT_EQ(initialized_versions > 0, monotonic);
  EXPECT_EQ(initializer_calls, 1U);
  EXPECT_EQ(changing_guards, 1U);
  test::RunFunctionPasses(function, AddInvariantCallPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
}

TEST_P(InitializedCallOptimizerTest, SpecializesIndependentGuardsAndPreservesUnprovenMetadata) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  const auto module = llvm::parseAssemblyString(R"(
declare void @fast.first()
declare void @host.first()
declare void @fast.second()
declare void @host.second()
declare void @varying.first()
declare void @varying.second()
define i64 @calculate(i1 %ready.first, i1 %ready.second, i64 %count) {
entry:
  br label %header
header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %established.first = phi i1 [ %ready.first, %entry ], [ true, %latch ]
  %established.second = phi i1 [ %ready.second, %entry ], [ true, %latch ]
  %more = icmp slt i64 %i, %count
  br i1 %more, label %first, label %done
first:
  br i1 %established.first, label %fast.first, label %host.first, !vela.call.invariant !0
fast.first:
  call void @fast.first()
  br label %second
host.first:
  call void @host.first()
  br label %second
second:
  br i1 %established.second, label %fast.second, label %host.second, !vela.call.invariant !0
fast.second:
  call void @fast.second()
  br label %varying
host.second:
  call void @host.second()
  br label %varying
varying:
  %changes = icmp eq i64 %i, 0
  br i1 %changes, label %varying.first, label %varying.second, !vela.call.invariant !0
varying.first:
  call void @varying.first()
  br label %latch
varying.second:
  call void @varying.second()
  br label %latch
latch:
  %next = add nsw i64 %i, 1
  br label %header
done:
  ret i64 %i
}
!0 = !{}
)",
                                                diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  auto& function = *module->getFunction("calculate");
  const auto monotonic = GetParam();
  if (!monotonic) {
    for (auto& block : function) {
      auto* branch = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
      if (block.getName() == "first") {
        branch->setCondition(function.getArg(0));
      } else if (block.getName() == "second") {
        branch->setCondition(function.getArg(1));
      }
    }
  }
  ASSERT_FALSE(llvm::verifyModule(*module));
  test::RunFunctionPasses(function, AddInvariantCallPasses);
  EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  // 两个独立条件均在快副本成立；慢路径仍须按各自状态执行宿主调用或初始化。
  for (const auto* name : {"fast.first", "fast.second", "host.first", "host.second"}) {
    const auto* target = module->getFunction(name);
    EXPECT_EQ(target->getNumUses(), target->getName().starts_with("host") ? 1U : 2U);
  }
  // 穷举入口状态，验证合取分流和慢路径没有跳过任一尚未 Ready 的调用。
  for (unsigned readiness = 0; readiness < 4; ++readiness) {
    llvm::ValueToValueMapTy mapped;
    auto* specialized = llvm::CloneFunction(&function, mapped);
    for (unsigned index = 0; index < 2; ++index) {
      const auto ready = (readiness & (1U << index)) != 0;
      specialized->getArg(index)->replaceAllUsesWith(llvm::ConstantInt::getBool(context, ready));
    }
    test::RunFunctionPasses(*specialized, AddInvariantCallPasses);
    EXPECT_FALSE(llvm::verifyFunction(*specialized));
    for (unsigned index = 0; index < 2; ++index) {
      const auto* target = module->getFunction(index == 0 ? "host.first" : "host.second");
      const auto calls = std::ranges::count_if(
          llvm::instructions(*specialized), [target](const auto& instruction) {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            return call != nullptr && call->getCalledFunction() == target;
          });
      EXPECT_EQ(calls, (readiness & (1U << index)) != 0 ? 0 : 1) << readiness;
    }
    specialized->eraseFromParent();
  }
  const auto unproven = std::ranges::count_if(function, [](const auto& block) {
    return block.getTerminator()->getMetadata(kNativeInvariantCallMetadata) != nullptr;
  });
  EXPECT_EQ(unproven, 2);
  const auto blocks = function.size();
  test::RunFunctionPasses(function, AddInvariantCallPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  EXPECT_EQ(function.size(), blocks);
}

INSTANTIATE_TEST_SUITE_P(InitializationState, InitializedCallOptimizerTest, testing::Bool());

TEST(InvariantCallOptimizerTest, CollectsSequentialLoopsBeforeChangingTheirSharedControlFlow) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  const auto module = llvm::parseAssemblyString(R"(
declare void @fast()
declare void @host()
define void @calculate(i1 %ready, i64 %count) {
entry:
  br label %first
first:
  %i = phi i64 [ 0, %entry ], [ %next.i, %first.latch ]
  %more.i = icmp slt i64 %i, %count
  br i1 %more.i, label %first.binding, label %between
first.binding:
  br i1 %ready, label %first.fast, label %first.host, !vela.call.invariant !0
first.fast:
  call void @fast()
  br label %first.latch
first.host:
  call void @host()
  br label %first.latch
first.latch:
  %next.i = add nsw i64 %i, 1
  br label %first
between:
  %ready.second = icmp slt i64 %i, 10
  br label %second
second:
  %j = phi i64 [ 0, %between ], [ %next.j, %second.latch ]
  %more.j = icmp slt i64 %j, %count
  br i1 %more.j, label %second.binding, label %done
second.binding:
  br i1 %ready.second, label %second.fast, label %second.host, !vela.call.invariant !0
second.fast:
  call void @fast()
  br label %second.latch
second.host:
  call void @host()
  br label %second.latch
second.latch:
  %next.j = add nsw i64 %j, 1
  br label %second
done:
  ret void
}
!0 = !{}
)",
                                                diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  auto& function = *module->getFunction("calculate");
  ASSERT_FALSE(llvm::verifyModule(*module));
  test::RunFunctionPasses(function, AddInvariantCallPasses);
  EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  EXPECT_EQ(module->getFunction("fast")->getNumUses(), 2U);
  EXPECT_EQ(module->getFunction("host")->getNumUses(), 2U);
  llvm::DominatorTree dominators(function);
  llvm::LoopInfo loops(dominators);
  EXPECT_EQ(std::distance(loops.begin(), loops.end()), 4);
  for (const auto& block : function) {
    EXPECT_EQ(block.getTerminator()->getMetadata(kNativeInvariantCallMetadata), nullptr);
  }
}

}  // namespace
}  // namespace vela::compiler::native
