#include "compiler/native/integer_loop_optimizer.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <tuple>

#include <gtest/gtest.h>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include "compiler/test_support/native_pass_test_support.h"

namespace vela::compiler::native {
namespace {

enum class LoopComparison : std::uint8_t { Signed, SameSign, Unsigned };

class IntegerLoopOptimizerTest
    : public testing::TestWithParam<std::tuple<bool, bool, LoopComparison>> {};

TEST_P(IntegerLoopOptimizerTest,
       UsesStepFlagsAndStrictSignedConditionsWithoutAssumingUnsignedBounds) {
  const auto [proven, descending, comparison] = GetParam();
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"(
declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)
declare void @raise(i64)
define i64 @sum(i64 %bound, i64 %initial, i64 %term) {
entry:
  br label %header
header:
  %index = phi i64 [ 0, %entry ], [ %next.index, %latch ]
  %total = phi i64 [ %initial, %entry ], [ %next.total, %latch ]
  %more = icmp slt i64 %index, %bound
  br i1 %more, label %body, label %done
body:
  %checked = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %total, i64 %term)
  %next.total = extractvalue { i64, i1 } %checked, 0
  %overflow = extractvalue { i64, i1 } %checked, 1
  br i1 %overflow, label %failed, label %latch
latch:
  %next.index = add i64 %index, 1
  br label %header
done:
  ret i64 %total
failed:
  %error.context = phi i64 [ %initial, %body ]
  call void @raise(i64 %error.context)
  ret i64 -1
}
)",
                                          diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  auto& function = *module->getFunction("sum");
  const auto signed_predicate = descending ? llvm::CmpInst::ICMP_SGT : llvm::CmpInst::ICMP_SLT;
  const auto predicate = comparison == LoopComparison::Signed
                             ? signed_predicate
                             : llvm::ICmpInst::getUnsignedPredicate(signed_predicate);
  const auto direction = descending ? -1 : 1;
  for (auto& block : function) {
    for (auto& instruction : block) {
      if (instruction.getName() == "next.index") {
        auto& step = llvm::cast<llvm::BinaryOperator>(instruction);
        step.setHasNoSignedWrap(proven);
        step.setOperand(1, llvm::ConstantInt::getSigned(step.getType(), direction));
      } else if (instruction.getName() == "more") {
        auto& condition = llvm::cast<llvm::ICmpInst>(instruction);
        condition.setPredicate(predicate);
        condition.setSameSign(comparison == LoopComparison::SameSign);
      }
    }
  }
  test::RunFunctionPasses(function, AddIntegerLoopPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  std::size_t checked_updates = 0;
  for (const auto& block : function) {
    for (const auto& instruction : block) {
      const auto* call = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
      if (call != nullptr && call->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow) {
        ++checked_updates;
      }
    }
  }
  std::string generated;
  llvm::raw_string_ostream output(generated);
  module->print(output, nullptr);
  // CVP 可从严格 signed 条件重新证明单位步进；未知 unsigned 上界仍可能跨越符号位。
  const auto bounded =
      comparison == LoopComparison::Signed || (proven && comparison == LoopComparison::SameSign);
  EXPECT_EQ(checked_updates, bounded ? 0U : 1U) << generated;
}

INSTANTIATE_TEST_SUITE_P(StepProofs,
                         IntegerLoopOptimizerTest,
                         testing::Combine(testing::Bool(),
                                          testing::Bool(),
                                          testing::Values(LoopComparison::Signed,
                                                          LoopComparison::SameSign,
                                                          LoopComparison::Unsigned)));

class BoundedReductionOptimizerTest
    : public testing::TestWithParam<std::tuple<bool, bool, bool, bool>> {};

TEST_P(BoundedReductionOptimizerTest, RequiresBoundsOnTheCountTermAndInitialValue) {
  const auto [bounded_count, bounded_term, known_initial, subtract] = GetParam();
  const std::string operation = subtract ? "ssub" : "sadd";
  const auto source =
      "declare { i64, i1 } @llvm." + operation + ".with.overflow.i64(i64, i64)\n" +
      "define i64 @sum(ptr %values, i64 %count, i64 %initial) {\nentry:\n" +
      (bounded_count ? "  %bound = and i64 %count, 4294967295\n" : "") +
      "  br label %header\nheader:\n" +
      "  %index = phi i64 [ 0, %entry ], [ %next.index, %latch ]\n" + "  %total = phi i64 [ " +
      (known_initial ? "0" : "%initial") + ", %entry ], [ %next.total, %latch ]\n" +
      "  %more = icmp slt i64 %index, " + (bounded_count ? "%bound" : "%count") +
      "\n  br i1 %more, label %body, label %done\nbody:\n" +
      "  %address = getelementptr i64, ptr %values, i64 %index\n" +
      "  %term = load i64, ptr %address, align 8" + (bounded_term ? ", !range !0" : "") +
      "\n  %checked = call { i64, i1 } @llvm." + operation +
      ".with.overflow.i64(i64 %total, i64 %term)\n" +
      "  %next.total = extractvalue { i64, i1 } %checked, 0\n" +
      "  %overflow = extractvalue { i64, i1 } %checked, 1\n" +
      "  br i1 %overflow, label %failed, label %latch\nlatch:\n" +
      "  %next.index = add nsw i64 %index, 1\n  br label %header\ndone:\n" +
      "  ret i64 %total\nfailed:\n  ret i64 -1\n}\n!0 = !{i64 -1024, i64 1025}\n";
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  const auto module = llvm::parseAssemblyString(source, diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  test::RunFunctionPasses(*module->getFunction("sum"), AddIntegerLoopPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  std::size_t checked_updates = 0;
  for (const auto& block : *module->getFunction("sum")) {
    for (const auto& instruction : block) {
      const auto* call = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
      if (call != nullptr && (call->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow ||
                              call->getIntrinsicID() == llvm::Intrinsic::ssub_with_overflow)) {
        ++checked_updates;
      }
    }
  }
  EXPECT_EQ(checked_updates, bounded_count && bounded_term && known_initial ? 0U : 1U);
}

INSTANTIATE_TEST_SUITE_P(
    RangeProofs,
    BoundedReductionOptimizerTest,
    testing::Combine(testing::Bool(), testing::Bool(), testing::Bool(), testing::Bool()));

TEST(BoundedReductionOptimizerTest, RetainsChecksWhenTheHeaderUpdatesBeforeTestingTheBound) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  const auto module = llvm::parseAssemblyString(R"(
declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)
define i64 @sum(ptr %values, i64 %count) {
entry:
  %bound = and i64 %count, 1
  br label %header
header:
  %index = phi i64 [ 0, %entry ], [ %next.index, %latch ]
  %total = phi i64 [ 9223372036854775806, %entry ], [ %next.total, %latch ]
  %term = load i64, ptr %values, align 8, !range !0
  %checked = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %total, i64 %term)
  %next.total = extractvalue { i64, i1 } %checked, 0
  %overflow = extractvalue { i64, i1 } %checked, 1
  br i1 %overflow, label %failed, label %condition
condition:
  %more = icmp slt i64 %index, %bound
  br i1 %more, label %latch, label %done
latch:
  %next.index = add nsw i64 %index, 1
  br label %header
done:
  ret i64 %next.total
failed:
  ret i64 -1
}
!0 = !{i64 0, i64 2}
)",
                                                diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  test::RunFunctionPasses(*module->getFunction("sum"), AddIntegerLoopPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  const auto* checked = module->getFunction("llvm.sadd.with.overflow.i64");
  ASSERT_NE(checked, nullptr);
  EXPECT_FALSE(checked->use_empty());
}

class BoundedPollOptimizerTest : public testing::TestWithParam<std::tuple<bool, bool, bool>> {};

bool HasUnpolledArrayLoad(const llvm::Loop& loop) {
  bool has_array_load = false;
  for (const auto* block : loop.blocks()) {
    for (const auto& instruction : *block) {
      if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (load->isAtomic()) {
          return false;
        }
        has_array_load = has_array_load ||
                         load->getType()->isIntegerTy(std::numeric_limits<std::uint64_t>::digits);
      }
    }
  }
  return has_array_load;
}

constexpr auto* kBoundedPollLoop = R"(
declare void @process()
declare void @host()
define i64 @sum(ptr %values, ptr %requests.address, i64 %count) {
entry:
  br label %header
header:
  %index = phi i64 [ 0, %entry ], [ %next.index, %calculate ]
  %total = phi i64 [ 0, %entry ], [ %next.total, %calculate ]
  %more = icmp slt i64 %index, %count
  br i1 %more, label %poll, label %done
poll:
  %requests = load atomic i8, ptr %requests.address monotonic, align 1, !vela.poll.ready.state !0
  %pending = icmp ne i8 %requests, 0
  br i1 %pending, label %process, label %calculate, !vela.poll.bounded !1
process:
  call void @process()
  br label %calculate
calculate:
  %address = getelementptr i64, ptr %values, i64 %index
  %value = load i64, ptr %address, align 8
  %next.total = xor i64 %total, %value
  %next.index = add nsw i64 %index, 1
  br label %header
done:
  ret i64 %total
}
!0 = !{i8 0}
!1 = !{}
)";

TEST_P(BoundedPollOptimizerTest, TracksReadyStateAfterBranchInversionAndRejectsObservableBodies) {
  const auto [inverted, observable, marked] = GetParam();
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(kBoundedPollLoop, diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  auto& function = *module->getFunction("sum");
  for (auto& block : function) {
    if (observable && block.getName() == "calculate") {
      llvm::IRBuilder<> builder(&*block.getFirstInsertionPt());
      builder.CreateCall(module->getFunction("host"));
    }
    for (auto& instruction : block) {
      if (!marked && instruction.getName() == "requests") {
        instruction.setMetadata(kNativePollReadyStateMetadata, nullptr);
      } else if (inverted && instruction.getName() == "pending") {
        llvm::cast<llvm::ICmpInst>(instruction).setPredicate(llvm::CmpInst::ICMP_EQ);
        llvm::cast<llvm::CondBrInst>(block.getTerminator())->swapSuccessors();
      }
    }
  }
  test::RunFunctionPasses(function, AddIntegerLoopPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  llvm::DominatorTree dominators(function);
  const llvm::LoopInfo loops(dominators);
  std::size_t bounded_loops = 0;
  for (const auto* loop : loops.getLoopsInPreorder()) {
    if (!loop->getSubLoops().empty()) {
      continue;
    }
    bounded_loops += HasUnpolledArrayLoad(*loop) ? 1 : 0;
  }
  EXPECT_EQ(bounded_loops != 0, marked && !observable);
}

INSTANTIATE_TEST_SUITE_P(PollProofs,
                         BoundedPollOptimizerTest,
                         testing::Combine(testing::Bool(), testing::Bool(), testing::Bool()));

class BoundedPollBudgetTest : public testing::TestWithParam<bool> {};

TEST_P(BoundedPollBudgetTest, CountsFastWorkWithoutChargingColdRequestHandling) {
  const bool fast_work = GetParam();
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(kBoundedPollLoop, diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  auto& function = *module->getFunction("sum");
  for (auto& block : function) {
    if (block.getName() == (fast_work ? "calculate" : "process")) {
      llvm::IRBuilder<> builder(block.getTerminator());
      // 保留足够多的真实指令，使快路径受工作量上限约束，冷请求处理则不缩短片段。
      constexpr std::size_t kExtraLoads = 256;
      for (std::size_t index = 0; index < kExtraLoads; ++index) {
        builder.CreateLoad(builder.getInt64Ty(), function.getArg(0))->setVolatile(true);
      }
    }
  }
  test::RunFunctionPasses(function, AddIntegerLoopPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  std::size_t chunk_guards = 0;
  for (const auto& block : function) {
    if (!block.getName().starts_with("poll.chunk")) {
      continue;
    }
    for (const auto& instruction : block) {
      const auto* comparison = llvm::dyn_cast<llvm::ICmpInst>(&instruction);
      if (comparison == nullptr || comparison->getPredicate() != llvm::CmpInst::ICMP_ULE) {
        continue;
      }
      const auto* limit = llvm::dyn_cast<llvm::ConstantInt>(comparison->getOperand(1));
      ASSERT_NE(limit, nullptr);
      ++chunk_guards;
      if (fast_work) {
        EXPECT_GT(limit->getZExtValue(), 0U);
        EXPECT_LT(limit->getZExtValue(), 1024U);
      } else {
        EXPECT_EQ(limit->getZExtValue(), 1024U);
      }
    }
  }
  EXPECT_EQ(chunk_guards, 1U);
}

INSTANTIATE_TEST_SUITE_P(WorkBudgets, BoundedPollBudgetTest, testing::Bool());

}  // namespace
}  // namespace vela::compiler::native
