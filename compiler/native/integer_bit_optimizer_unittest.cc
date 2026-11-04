#include "compiler/native/integer_bit_optimizer.h"

#include <cstddef>

#include <gtest/gtest.h>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
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

namespace vela::compiler::native {
namespace {

TEST(IntegerBitOptimizerTest, RequiresExactMasksComplementaryShiftsAndProvenZeroHighBits) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  const auto module = llvm::parseAssemblyString(R"(
define i64 @masked(i64 %value) {
  %input = and i64 %value, 4294967295
  %right = lshr i64 %input, 7
  %left = shl i64 %input, 25
  %combined = or i64 %right, %left
  %result = and i64 4294967295, %combined
  ret i64 %result
}
define i64 @distributed(i64 %value) {
  %input = and i64 %value, 4294967295
  %right = lshr i64 %input, 7
  %left = shl i64 %input, 25
  %masked = and i64 %left, 4261412864
  %result = or i64 %masked, %right
  ret i64 %result
}
define i64 @byte(i64 %value) {
  %input = and i64 %value, 255
  %right = lshr i64 %input, 3
  %left = shl i64 %input, 5
  %combined = or i64 %right, %left
  %result = and i64 %combined, 255
  ret i64 %result
}
define i64 @short(i64 %value) {
  %input = and i64 %value, 65535
  %right = lshr i64 %input, 7
  %left = shl i64 %input, 9
  %combined = or i64 %right, %left
  %result = and i64 %combined, 65535
  ret i64 %result
}
define i64 @unknown(i64 %input) {
  %right = lshr i64 %input, 7
  %left = shl i64 %input, 25
  %combined = or i64 %right, %left
  %result = and i64 %combined, 4294967295
  ret i64 %result
}
define i64 @incomplete(i64 %value) {
  %input = and i64 %value, 4294967295
  %right = lshr i64 %input, 7
  %left = shl i64 %input, 25
  %combined = or i64 %right, %left
  %result = and i64 %combined, 4294967294
  ret i64 %result
}
define i64 @different(i64 %value, i64 %other) {
  %input = and i64 %value, 4294967295
  %right = lshr i64 %input, 7
  %left = shl i64 %other, 25
  %combined = or i64 %right, %left
  %result = and i64 %combined, 4294967295
  ret i64 %result
}
define i64 @wrong_distance(i64 %value) {
  %input = and i64 %value, 4294967295
  %right = lshr i64 %input, 8
  %left = shl i64 %input, 25
  %combined = or i64 %right, %left
  %result = and i64 %combined, 4294967295
  ret i64 %result
}
)",
                                                diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  llvm::FunctionAnalysisManager analyses;
  llvm::PassBuilder builder;
  builder.registerFunctionAnalyses(analyses);
  llvm::FunctionPassManager passes;
  AddIntegerBitPasses(passes);
  for (auto& function : *module) {
    if (!function.isDeclaration()) {
      passes.run(function, analyses);
    }
  }
  EXPECT_FALSE(llvm::verifyModule(*module));
  std::size_t rotations = 0;
  for (auto& function : *module) {
    for (const auto& instruction : llvm::instructions(function)) {
      const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
      if (intrinsic != nullptr && intrinsic->getIntrinsicID() == llvm::Intrinsic::fshr) {
        EXPECT_TRUE(function.getName() == "masked" || function.getName() == "distributed" ||
                    function.getName() == "byte" || function.getName() == "short");
        ++rotations;
      }
    }
  }
  EXPECT_EQ(rotations, 4U);
}

}  // namespace
}  // namespace vela::compiler::native
