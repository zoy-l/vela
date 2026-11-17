#include "compiler/native/array_reduction_optimizer.h"

#include <cstddef>
#include <initializer_list>
#include <memory>

#include <gtest/gtest.h>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueSymbolTable.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>

#include "compiler/native/memory_regions.h"
#include "compiler/test_support/native_pass_test_support.h"
#include "runtime/native/native_memory_layout.h"

namespace vela::compiler::native {
namespace {

class ArrayReductionOptimizerTest : public testing::TestWithParam<bool> {};

TEST(ArrayReductionShapeTest, RejectsLoopsThatSimplifyCannotGiveAPreheader) {
  llvm::LLVMContext context;
  const auto module = test::ParseModule(R"(
define void @indirect(ptr %destination, i1 %again) {
entry:
  indirectbr ptr %destination, [label %header, label %exit]
header:
  br label %latch
latch:
  br i1 %again, label %header, label %exit
exit:
  ret void
}
)",
                                        context);
  ASSERT_NE(module, nullptr);
  auto& function = *module->getFunction("indirect");
  function.addFnAttr(kNativeArrayReductionCandidateAttribute);
  test::RunFunctionPasses(function, AddArrayReductionPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  test::WithFunctionLoops(*module, [&](auto&, const auto& loops) {
    ASSERT_FALSE(loops.empty());
    EXPECT_EQ((*loops.begin())->getLoopPreheader(), nullptr);
  });
}
TEST_P(ArrayReductionOptimizerTest, ChecksTheSliceOwnerAndNeverRetriesTheCheckedFallback) {
  llvm::LLVMContext context;
  auto module = test::ParseModule(R"(
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)
define i64 @sum(ptr %array, ptr %other, i64 %start, i64 %bound) {
entry:
  %length.address = getelementptr i8, ptr %array, i64 0
  %length = load i32, ptr %length.address, align 4
  %data.address = getelementptr i8, ptr %array, i64 0
  %data = load ptr, ptr %data.address, align 8
  br label %header
header:
  %index = phi i64 [ %start, %entry ], [ %next, %latch ]
  %total = phi i64 [ 0, %entry ], [ %sum, %latch ]
  %address = getelementptr i64, ptr %data, i64 %index
  %element = load i64, ptr %address, align 8
  %update = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %total, i64 %element)
  %sum = extractvalue { i64, i1 } %update, 0
  %overflow = extractvalue { i64, i1 } %update, 1
  br i1 %overflow, label %failed, label %latch
latch:
  %next = add nsw i64 %index, 1
  %more = icmp ne i64 %next, %bound
  br i1 %more, label %header, label %done
failed:
  ret i64 -1
done:
  ret i64 %sum
}
)",
                                  context);
  ASSERT_NE(module, nullptr);
  auto& function = *module->getFunction("sum");
  function.addFnAttr(kNativeArrayReductionCandidateAttribute);
  const auto& symbols = *function.getValueSymbolTable();
  auto* length_address = llvm::cast<llvm::GetElementPtrInst>(symbols.lookup("length.address"));
  auto* data_address = llvm::cast<llvm::GetElementPtrInst>(symbols.lookup("data.address"));
  auto* word = llvm::Type::getInt64Ty(context);
  length_address->setOperand(
      1, llvm::ConstantInt::get(word, runtime::NativeMemoryLayout::kObjectLength));
  data_address->setOperand(1,
                           llvm::ConstantInt::get(word, runtime::NativeMemoryLayout::kArrayData));
  const auto other_owner = GetParam();
  if (other_owner) {
    length_address->setOperand(0, function.getArg(1));
  }
  for (const auto* name : {"length", "data"}) {
    llvm::cast<llvm::Instruction>(symbols.lookup(name))
        ->setMetadata(llvm::LLVMContext::MD_tbaa,
                      NativeMemoryTag(context, MemoryRegion::HeapHeader));
  }
  llvm::cast<llvm::Instruction>(symbols.lookup("element"))
      ->setMetadata(llvm::LLVMContext::MD_tbaa, NativeMemoryTag(context, MemoryRegion::Elements));
  test::RunFunctionPasses(function, AddArrayReductionPasses);
  ASSERT_FALSE(llvm::verifyModule(*module));
  std::size_t before = 0;
  std::size_t fallbacks = 0;
  test::WithFunctionLoops(*module, [&](auto&, const auto& loops) {
    before = loops.getLoopsInPreorder().size();
    for (const auto* loop : loops.getLoopsInPreorder()) {
      fallbacks +=
          llvm::findOptionMDForLoop(loop, kNativeArrayReductionFallbackMetadata) != nullptr ? 1 : 0;
    }
  });
  EXPECT_EQ(fallbacks, other_owner ? 0U : 1U);
  if (!other_owner) {
    EXPECT_GT(before, 1U);
  }
  test::RunFunctionPasses(function, AddArrayReductionPasses);
  ASSERT_FALSE(llvm::verifyModule(*module));
  test::WithFunctionLoops(*module, [&](auto&, const auto& loops) {
    EXPECT_EQ(loops.getLoopsInPreorder().size(), before);
  });
}

INSTANTIATE_TEST_SUITE_P(SliceOwners, ArrayReductionOptimizerTest, testing::Bool());

}  // namespace
}  // namespace vela::compiler::native
