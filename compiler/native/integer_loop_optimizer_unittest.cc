#include "compiler/native/integer_loop_optimizer.h"

#include <cstddef>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
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

TEST(IntegerLoopOptimizerTest, RecognizesProvenStepsWithoutAssumingWrappingStepsAreBounded) {
  for (const bool proven : {false, true}) {
    for (const bool descending : {false, true}) {
      SCOPED_TRACE(proven);
      SCOPED_TRACE(descending);
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
      for (auto& block : function) {
        for (auto& instruction : block) {
          if (instruction.getName() == "next.index") {
            auto& step = llvm::cast<llvm::BinaryOperator>(instruction);
            step.setHasNoSignedWrap(proven);
            if (descending) {
              step.setOperand(1, llvm::ConstantInt::getSigned(step.getType(), -1));
            }
          } else if (descending && instruction.getName() == "more") {
            llvm::cast<llvm::ICmpInst>(instruction).setPredicate(llvm::CmpInst::ICMP_SGT);
          }
        }
      }
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
      EXPECT_EQ(checked_updates, proven ? 0U : 1U) << generated;
    }
  }
}

}  // namespace
}  // namespace vela::compiler::native
