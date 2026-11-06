#include "compiler/native/integer_loop_optimizer.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <tuple>

#include <gtest/gtest.h>

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

namespace vela::compiler::native {
namespace {

void OptimizeLoopFunction(llvm::Function& function) {
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager calls;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder builder;
  builder.registerModuleAnalyses(modules);
  builder.registerCGSCCAnalyses(calls);
  builder.registerFunctionAnalyses(functions);
  builder.registerLoopAnalyses(loops);
  builder.crossRegisterProxies(loops, functions, calls, modules);
  llvm::FunctionPassManager passes;
  AddIntegerLoopPasses(passes);
  passes.run(function, functions);
}

enum class LoopComparison : std::uint8_t { Signed, SameSign, Unsigned };

class IntegerLoopOptimizerTest
    : public testing::TestWithParam<std::tuple<bool, bool, LoopComparison>> {};

TEST_P(IntegerLoopOptimizerTest, RecognizesProvenStepsWithoutAssumingWrappingStepsAreBounded) {
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
  OptimizeLoopFunction(function);
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
  const auto bounded = proven && comparison != LoopComparison::Unsigned;
  EXPECT_EQ(checked_updates, bounded ? 0U : 1U) << generated;
}

INSTANTIATE_TEST_SUITE_P(StepProofs,
                         IntegerLoopOptimizerTest,
                         testing::Combine(testing::Bool(),
                                          testing::Bool(),
                                          testing::Values(LoopComparison::Signed,
                                                          LoopComparison::SameSign,
                                                          LoopComparison::Unsigned)));

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

TEST_P(BoundedPollOptimizerTest, TracksReadyStateAfterBranchInversionAndRejectsObservableBodies) {
  const auto [inverted, observable, marked] = GetParam();
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"(
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
  call void @host()
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
)",
                                          diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  auto& function = *module->getFunction("sum");
  for (auto& block : function) {
    for (auto iterator = block.begin(); iterator != block.end();) {
      auto& instruction = *iterator++;
      if (!marked && instruction.getName() == "requests") {
        instruction.setMetadata(kNativePollReadyStateMetadata, nullptr);
      } else if (inverted && instruction.getName() == "pending") {
        llvm::cast<llvm::ICmpInst>(instruction).setPredicate(llvm::CmpInst::ICMP_EQ);
        llvm::cast<llvm::BranchInst>(block.getTerminator())->swapSuccessors();
      } else if (auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                 !observable && call != nullptr && call->getCalledFunction()->getName() == "host") {
        instruction.eraseFromParent();
      }
    }
  }
  OptimizeLoopFunction(function);
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

}  // namespace
}  // namespace vela::compiler::native
