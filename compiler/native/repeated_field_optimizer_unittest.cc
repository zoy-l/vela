#include "compiler/native/repeated_field_optimizer.h"

#include <string>
#include <string_view>
#include <tuple>

#include <gtest/gtest.h>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>

#include "base/declaration.h"
#include "compiler/native/memory_regions.h"
#include "compiler/test_support/native_pass_test_support.h"

namespace vela::compiler::native {
namespace {

class RepeatedFieldPipelineTest
    : public testing::TestWithParam<std::tuple<std::string_view, DeclarationId>> {};

TEST_P(RepeatedFieldPipelineTest, PreparesOnlyPotentiallyEligibleTripCounts) {
  const auto [trip_count, field] = GetParam();
  llvm::LLVMContext context;
  const auto source = std::string(R"(
define void @update(ptr %fields, i64 %count) {
entry:
  %bound = alloca i64
  store i64 10, ptr %bound
  %loaded = load i64, ptr %bound
  br label %outer
outer:
  %round = phi i64 [ 0, %entry ], [ %next.round, %outer.latch ]
  %more = icmp slt i64 %round, )") +
                      std::string(trip_count) + R"(
  br i1 %more, label %inner.preheader, label %done
inner.preheader:
  br label %inner
inner:
  %index = phi i64 [ 0, %inner.preheader ], [ %next.index, %inner ]
  %identity = add i64 %index, 0
  %address = getelementptr double, ptr %fields, i64 %identity
  store double 1.0, ptr %address
  %next.index = add nuw nsw i64 %index, 1
  %inner.more = icmp ult i64 %next.index, 4
  br i1 %inner.more, label %inner, label %outer.latch
outer.latch:
  %next.round = add nuw nsw i64 %round, 1
  br label %outer
done:
  ret void
}
)";
  const auto module = test::ParseModule(source, context);
  ASSERT_NE(module, nullptr);
  auto& function = *module->getFunction("update");
  auto* tag = NativeMemoryTag(context, MemoryRegion::FieldPayloads, field);
  for (auto& block : function) {
    for (auto& instruction : block) {
      if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
          store != nullptr && store->getValueOperand()->getType()->isDoubleTy()) {
        store->setMetadata(llvm::LLVMContext::MD_tbaa, tag);
      }
    }
  }
  std::string before;
  llvm::raw_string_ostream original(before);
  module->print(original, nullptr);
  test::RunFunctionPasses(function, AddRepeatedFieldPasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  std::string after;
  llvm::raw_string_ostream optimized(after);
  module->print(optimized, nullptr);
  if (trip_count == "%loaded") {
    EXPECT_NE(before, after);
  } else {
    EXPECT_EQ(before, after);
  }
}

INSTANTIATE_TEST_SUITE_P(
    TripCountsAndFields,
    RepeatedFieldPipelineTest,
    testing::Combine(testing::Values("0", "1", "33", "64", "%count", "%loaded"),
                     testing::Values(DeclarationId{}, DeclarationId{1})));

}  // namespace
}  // namespace vela::compiler::native
