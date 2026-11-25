#include "compiler/native/array_reduction_optimizer.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueSymbolTable.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Utils/LoopUtils.h>

#include "compiler/native/memory_regions.h"
#include "compiler/test_support/native_pass_test_support.h"
#include "runtime/native/native_memory_layout.h"

namespace vela::compiler::native {
namespace {

class ArrayReductionOptimizerTest : public testing::TestWithParam<bool> {};

TEST(ArrayReductionShapeTest, ReplacesOnlyProvenContiguousActiveGatherLanes) {
  const auto prefix = std::string(R"(
target datalayout = "e-p:64:64"
declare <4 x i64> @llvm.masked.gather.v4i64.v4p0(<4 x ptr>, <4 x i1>, <4 x i64>)
define <4 x i64> @read(ptr %values, <4 x i1> %mask) {
entry:
  %address = getelementptr )");
  const auto suffix = std::string(R"(
  %value = call <4 x i64> @llvm.masked.gather.v4i64.v4p0(<4 x ptr> align 8 %address,
)");
  const auto end = std::string(R"(, <4 x i64> poison), !tbaa !2
  ret <4 x i64> %value
}
!0 = !{!"Vela native storage"}
!1 = !{!"array elements", !0}
!2 = !{!1, !1, i64 0}
)");
  for (const auto offset : {0, 2}) {
    for (const auto masked : {false, true}) {
      for (const auto inbounds : {false, true}) {
        llvm::LLVMContext context;
        const auto* const indices =
            offset == 0 ? "<i64 0, i64 1, i64 2, i64 3>" : "<i64 0, i64 2, i64 4, i64 6>";
        auto source = prefix;
        source.append(inbounds ? "inbounds " : "");
        source.append("[8 x i8], ptr %values, <4 x i64> ");
        source.append(indices);
        source.append(suffix);
        source.append(masked ? "<4 x i1> %mask" : "<4 x i1> splat (i1 true)");
        source.append(end);
        const auto module = test::ParseModule(source, context);
        ASSERT_NE(module, nullptr);
        auto& function = *module->getFunction("read");
        test::RunFunctionPasses(function, AddArrayReductionPasses);
        EXPECT_FALSE(llvm::verifyModule(*module));
        const auto has_gather =
            std::ranges::any_of(llvm::instructions(function), [](const auto& item) {
              const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&item);
              return intrinsic != nullptr &&
                     intrinsic->getIntrinsicID() == llvm::Intrinsic::masked_gather;
            });
        EXPECT_EQ(has_gather, offset != 0 || masked || !inbounds);
      }
    }
  }
}

TEST(ArrayReductionShapeTest, ChecksEveryPhiEntryAndBackedgeBeforeReplacingGather) {
  for (const auto mixed_seed : {false, true}) {
    for (const auto mixed_step : {false, true}) {
      llvm::LLVMContext context;
      const std::string seed =
          mixed_seed ? "<i64 0, i64 2, i64 4, i64 6>" : "<i64 8, i64 9, i64 10, i64 11>";
      const std::string step = mixed_step ? "<i64 4, i64 4, i64 8, i64 4>" : "splat (i64 4)";
      const auto module = test::ParseModule(std::string(R"(
target datalayout = "e-p:64:64"
declare <4 x i64> @llvm.masked.gather.v4i64.v4p0(<4 x ptr>, <4 x i1>, <4 x i64>)
define <4 x i64> @read(ptr %values, i1 %choose, i1 %again) {
entry:
  br i1 %choose, label %first, label %second
first:
  br label %loop
second:
  br label %loop
loop:
  %indices = phi <4 x i64> [ <i64 0, i64 1, i64 2, i64 3>, %first ], [ )")
                                                .append(seed)
                                                .append(R"(, %second ], [ %next, %loop ]
  %address = getelementptr inbounds i64, ptr %values, <4 x i64> %indices
  %value = call <4 x i64> @llvm.masked.gather.v4i64.v4p0(<4 x ptr> align 8 %address,
      <4 x i1> splat (i1 true), <4 x i64> poison), !tbaa !2
  %next = add <4 x i64> %indices, )")
                                                .append(step)
                                                .append(R"(
  br i1 %again, label %loop, label %done
done:
  ret <4 x i64> %value
}
!0 = !{!"Vela native storage"}
!1 = !{!"array elements", !0}
!2 = !{!1, !1, i64 0}
)"),
                                            context);
      ASSERT_NE(module, nullptr);
      auto& function = *module->getFunction("read");
      test::RunFunctionPasses(function, AddArrayReductionPasses);
      EXPECT_FALSE(llvm::verifyModule(*module));
      const auto* gather = module->getFunction("llvm.masked.gather.v4i64.v4p0");
      EXPECT_EQ(gather != nullptr && !gather->use_empty(), mixed_seed || mixed_step);
    }
  }
}

TEST(ArrayReductionShapeTest, KeepsGatherAcrossNarrowIndexSignExtensionWrap) {
  llvm::LLVMContext context;
  const auto module = test::ParseModule(R"(
target datalayout = "e-p:64:64"
declare <4 x i64> @llvm.masked.gather.v4i64.v4p0(<4 x ptr>, <4 x i1>, <4 x i64>)
define <4 x i64> @read(ptr %values) {
entry:
  %address = getelementptr inbounds i64, ptr %values,
      <4 x i32> <i32 2147483646, i32 2147483647, i32 -2147483648, i32 -2147483647>
  %value = call <4 x i64> @llvm.masked.gather.v4i64.v4p0(<4 x ptr> align 8 %address,
      <4 x i1> splat (i1 true), <4 x i64> poison), !tbaa !2
  ret <4 x i64> %value
}
!0 = !{!"Vela native storage"}
!1 = !{!"array elements", !0}
!2 = !{!1, !1, i64 0}
)",
                                        context);
  ASSERT_NE(module, nullptr);
  test::RunFunctionPasses(*module->getFunction("read"), AddArrayReductionPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  const auto* gather = module->getFunction("llvm.masked.gather.v4i64.v4p0");
  ASSERT_NE(gather, nullptr);
  EXPECT_FALSE(gather->use_empty());
}

TEST(ArrayReductionShapeTest, LimitsArrayExpansionWithoutReplacingExplicitLoopHints) {
  for (const auto hinted : {false, true}) {
    std::string source = R"(
target datalayout = "e-p:64:64"
define i64 @read(ptr %values) {
entry:
  br label %loop
loop:
  %index = phi i64 [ 0, %entry ], [ %next, %loop ]
)";
    constexpr unsigned kReads = 16;
    for (unsigned index = 0; index < kReads; ++index) {
      source += "  %read" + std::to_string(index) + " = load volatile i64, ptr %values, !tbaa !2\n";
    }
    source += R"(
  %next = add nuw nsw i64 %index, 1
  %more = icmp ult i64 %next, 64
  br i1 %more, label %loop, label %done
done:
  ret i64 %read0
}
!0 = !{!"Vela native storage"}
!1 = !{!"array elements", !0}
!2 = !{!1, !1, i64 0}
)";
    llvm::LLVMContext context;
    const auto module = test::ParseModule(source, context);
    ASSERT_NE(module, nullptr);
    if (hinted) {
      test::WithFunctionLoops(*module, [](auto&, const auto& loops) {
        llvm::addStringMetadataToLoop(*loops.begin(), "llvm.loop.unroll.disable", 1);
      });
    }
    test::RunFunctionPasses(*module->getFunction("read"), AddArrayReductionPasses);
    EXPECT_FALSE(llvm::verifyModule(*module));
    test::WithFunctionLoops(*module, [&](auto&, const auto& loops) {
      const auto* loop = *loops.begin();
      const auto* count = llvm::findOptionMDForLoop(loop, "llvm.loop.unroll.count");
      if (hinted) {
        EXPECT_EQ(count, nullptr);
        EXPECT_NE(llvm::findOptionMDForLoop(loop, "llvm.loop.unroll.disable"), nullptr);
      } else {
        ASSERT_NE(count, nullptr);
        EXPECT_TRUE(llvm::mdconst::extract<llvm::ConstantInt>(count->getOperand(1))->equalsInt(4));
      }
    });
  }
}

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
