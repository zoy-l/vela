#include "compiler/native/recursive_inliner.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>

#include "compiler/test_support/native_pass_test_support.h"

namespace vela::compiler::native {
namespace {

llvm::Function& RecursiveFunction(llvm::Module& module, std::size_t padding) {
  auto& context = module.getContext();
  llvm::IRBuilder<> builder(context);
  auto* word = builder.getInt64Ty();
  auto* type = llvm::FunctionType::get(word, {word}, false);
  auto* function =
      llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "recursive", module);
  auto* entry = llvm::BasicBlock::Create(context, "entry", function);
  auto* base = llvm::BasicBlock::Create(context, "base", function);
  auto* recurse = llvm::BasicBlock::Create(context, "recurse", function);
  auto* input = function->getArg(0);
  builder.SetInsertPoint(entry);
  builder.CreateCondBr(builder.CreateICmpSLT(input, builder.getInt64(2)), base, recurse);
  builder.SetInsertPoint(base);
  builder.CreateRet(input);
  builder.SetInsertPoint(recurse);
  auto* first = builder.CreateCall(function, {builder.CreateSub(input, builder.getInt64(1))});
  auto* second = builder.CreateCall(function, {builder.CreateSub(input, builder.getInt64(2))});
  auto* result = builder.CreateAdd(first, second);
  for (std::size_t index = 0; index < padding; ++index) {
    result = builder.CreateAdd(result, input);
  }
  builder.CreateRet(result);
  return *function;
}

std::size_t RecursiveCallCount(const llvm::Function& function) {
  std::size_t calls = 0;
  for (const auto& instruction : llvm::instructions(function)) {
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (call != nullptr && call->getCalledFunction() == &function) {
      ++calls;
    }
  }
  return calls;
}

TEST(RecursiveInlinerTest, ExpandsThreeLayersAndPreservesFunctionIdentityAndLinkage) {
  llvm::LLVMContext context;
  llvm::Module module("recursive", context);
  auto& function = RecursiveFunction(module, 0);
  const auto* identity = &function;
  InlineRecursion(function);
  EXPECT_FALSE(llvm::verifyModule(module));
  EXPECT_EQ(module.getFunction("recursive"), identity);
  EXPECT_EQ(function.getLinkage(), llvm::GlobalValue::InternalLinkage);
  EXPECT_EQ(RecursiveCallCount(function), 16U);
  EXPECT_LE(function.getInstructionCount(), kMaximumRecursiveInlineInstructions);
  EXPECT_EQ(module.size(), 1U);
}

TEST(RecursiveInlinerTest, DiscardsTheWholeLayerThatExceedsTheLoweredInstructionBudget) {
  llvm::LLVMContext context;
  llvm::Module module("recursive", context);
  constexpr std::size_t kPadding = kMaximumRecursiveInlineInstructions / 10;
  auto& function = RecursiveFunction(module, kPadding);
  InlineRecursion(function);
  EXPECT_FALSE(llvm::verifyModule(module));
  EXPECT_EQ(RecursiveCallCount(function), 8U);
  EXPECT_LE(function.getInstructionCount(), kMaximumRecursiveInlineInstructions);
  EXPECT_EQ(module.size(), 1U);
}

TEST(RecursiveInlinerTest, LeavesAnAlreadyOversizedBodyUnchanged) {
  llvm::LLVMContext context;
  llvm::Module module("recursive", context);
  auto& function = RecursiveFunction(module, kMaximumRecursiveInlineInstructions);
  const auto original_count = function.getInstructionCount();
  InlineRecursion(function);
  EXPECT_FALSE(llvm::verifyModule(module));
  EXPECT_EQ(function.getInstructionCount(), original_count);
  EXPECT_EQ(RecursiveCallCount(function), 2U);
  EXPECT_EQ(module.size(), 1U);
}

TEST(RecursiveLoopHintsTest, RequiresProvenShortTripsAndBoundsExpandedCode) {
  struct Case final {
    std::optional<std::uint64_t> limit;
    std::size_t padding;
    std::uint64_t expected_count;
  };
  constexpr std::array kCases{
      Case{.limit = 4, .padding = 0, .expected_count = 4},
      Case{.limit = 16, .padding = 0, .expected_count = 0},
      Case{.limit = std::nullopt, .padding = 0, .expected_count = 0},
      Case{.limit = 4, .padding = kMaximumRecursiveInlineInstructions / 4, .expected_count = 0},
  };
  for (const auto& entry : kCases) {
    llvm::LLVMContext context;
    llvm::Module module("loop_hints", context);
    llvm::IRBuilder<> builder(context);
    auto* word = builder.getInt64Ty();
    auto* type = llvm::FunctionType::get(word, {word}, false);
    auto* function =
        llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "recursive", module);
    auto* start = llvm::BasicBlock::Create(context, "entry", function);
    auto* base = llvm::BasicBlock::Create(context, "base", function);
    auto* loop = llvm::BasicBlock::Create(context, "loop", function);
    auto* done = llvm::BasicBlock::Create(context, "done", function);
    auto* input = function->getArg(0);
    builder.SetInsertPoint(start);
    auto* reduced = builder.CreateSub(input, builder.getInt64(1));
    builder.CreateCondBr(builder.CreateICmpSLT(input, builder.getInt64(1)), base, loop);
    builder.SetInsertPoint(base);
    builder.CreateRet(builder.getInt64(0));
    builder.SetInsertPoint(loop);
    auto* index = builder.CreatePHI(word, 2);
    auto* total = builder.CreatePHI(word, 2);
    index->addIncoming(builder.getInt64(0), start);
    total->addIncoming(builder.getInt64(0), start);
    auto* call = builder.CreateCall(function, {reduced});
    auto* sum = builder.CreateAdd(total, call);
    for (std::size_t padding = 0; padding < entry.padding; ++padding) {
      sum = builder.CreateAdd(sum, input);
    }
    auto* next = builder.CreateAdd(index, builder.getInt64(1));
    index->addIncoming(next, loop);
    total->addIncoming(sum, loop);
    llvm::Value* limit = input;
    const auto constant_limit = entry.limit;
    if (constant_limit) {
      limit = builder.getInt64(*constant_limit);
    }
    builder.CreateCondBr(builder.CreateICmpSLT(next, limit), loop, done);
    builder.SetInsertPoint(done);
    builder.CreateRet(sum);
    const auto original_count = function->getInstructionCount();
    test::RunFunctionPasses(*function, AddRecursiveLoopHints);
    EXPECT_FALSE(llvm::verifyModule(module));
    EXPECT_EQ(function->getInstructionCount(), original_count);
    EXPECT_EQ(RecursiveCallCount(*function), 1U);
    EXPECT_EQ(call->hasFnAttr(llvm::Attribute::NoInline), entry.expected_count != 0);
    llvm::DominatorTree dominators(*function);
    const llvm::LoopInfo loops(dominators);
    const auto* hint = llvm::findOptionMDForLoop(loops.getLoopFor(loop), "llvm.loop.unroll.count");
    if (entry.expected_count == 0) {
      EXPECT_EQ(hint, nullptr);
    } else {
      ASSERT_NE(hint, nullptr);
      const auto* count = llvm::mdconst::dyn_extract<llvm::ConstantInt>(hint->getOperand(1));
      ASSERT_NE(count, nullptr);
      EXPECT_EQ(count->getZExtValue(), entry.expected_count);
    }
  }
}

TEST(RecursiveLoopHintsTest, CountsNestedCopiesAndPreservesTheirOwnUnrollPolicy) {
  struct Case final {
    std::optional<std::uint64_t> inner_limit;
    std::size_t padding;
    bool disabled;
    bool expected;
  };
  constexpr std::array kCases{
      Case{.inner_limit = 4, .padding = 0, .disabled = false, .expected = true},
      Case{.inner_limit = 4, .padding = 0, .disabled = true, .expected = true},
      Case{.inner_limit = std::nullopt, .padding = 0, .disabled = false, .expected = false},
      Case{.inner_limit = 1024, .padding = 0, .disabled = false, .expected = false},
      Case{.inner_limit = 4, .padding = 256, .disabled = false, .expected = false},
  };
  for (const auto& entry : kCases) {
    std::string source = R"(
define i64 @recursive(i64 %depth, i64 %bound, ptr %data) {
entry:
  %base = icmp eq i64 %depth, 0
  br i1 %base, label %base.return, label %outer
base.return:
  ret i64 0
outer:
  %index = phi i64 [ 0, %entry ], [ %next, %inner.done ]
  br label %inner
inner:
  %scan = phi i64 [ 0, %outer ], [ %scan.next, %inner ]
)";
    for (std::size_t index = 0; index < entry.padding; ++index) {
      source += "  %padding" + std::to_string(index) + " = load volatile i64, ptr %data\n";
    }
    source += "  %scan.next = add nuw nsw i64 %scan, 1\n";
    source += "  %scan.more = icmp ult i64 %scan.next, ";
    const auto inner_limit = entry.inner_limit;
    source += inner_limit ? std::to_string(*inner_limit) : "%bound";
    source += "\n  br i1 %scan.more, label %inner, label %inner.done";
    source += entry.disabled ? ", !llvm.loop !0\n" : "\n";
    source += R"(
inner.done:
  %reduced = sub i64 %depth, 1
  %result = call i64 @recursive(i64 %reduced, i64 %bound, ptr %data)
  %next = add nuw nsw i64 %index, 1
  %more = icmp ult i64 %next, 4
  br i1 %more, label %outer, label %done
done:
  ret i64 %result
}
!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.unroll.disable"}
)";
    llvm::LLVMContext context;
    const auto module = test::ParseModule(source, context);
    ASSERT_NE(module, nullptr);
    auto& function = *module->getFunction("recursive");
    test::RunFunctionPasses(function, AddRecursiveLoopHints);
    EXPECT_FALSE(llvm::verifyModule(*module));
    llvm::DominatorTree dominators(function);
    const llvm::LoopInfo loops(dominators);
    ASSERT_EQ(loops.getTopLevelLoops().size(), 1U);
    const auto* outer = *loops.begin();
    ASSERT_EQ(outer->getSubLoops().size(), 1U);
    const auto* inner = outer->getSubLoops().front();
    EXPECT_EQ(llvm::findOptionMDForLoop(outer, "llvm.loop.unroll.count") != nullptr,
              entry.expected);
    EXPECT_EQ(llvm::findOptionMDForLoop(inner, "llvm.loop.unroll.disable") != nullptr,
              entry.disabled);
    EXPECT_EQ(RecursiveCallCount(function), 1U);
  }
}

class LoopRecursiveInlinerTest : public testing::TestWithParam<bool> {};

TEST_P(LoopRecursiveInlinerTest, AvoidsNestedLoopExpansionButAllowsSequentialLoops) {
  llvm::LLVMContext context;
  llvm::Module module("loop_recursive", context);
  llvm::IRBuilder<> builder(context);
  auto* word = builder.getInt64Ty();
  auto* type = llvm::FunctionType::get(word, {word}, false);
  auto* function =
      llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "recursive", module);
  auto* entry = llvm::BasicBlock::Create(context, "entry", function);
  auto* base = llvm::BasicBlock::Create(context, "base", function);
  auto* loop = llvm::BasicBlock::Create(context, "loop", function);
  auto* done = llvm::BasicBlock::Create(context, "done", function);
  auto* input = function->getArg(0);
  builder.SetInsertPoint(entry);
  auto* reduced = builder.CreateSub(input, builder.getInt64(1));
  builder.CreateCondBr(builder.CreateICmpSLT(input, builder.getInt64(1)), base, loop);
  builder.SetInsertPoint(base);
  builder.CreateRet(builder.getInt64(0));
  builder.SetInsertPoint(loop);
  auto* index = builder.CreatePHI(word, 2);
  auto* total = builder.CreatePHI(word, 2);
  index->addIncoming(builder.getInt64(0), entry);
  total->addIncoming(builder.getInt64(0), entry);
  const auto recursion_in_loop = GetParam();
  auto* term = recursion_in_loop ? builder.CreateCall(function, {reduced}) : reduced;
  auto* sum = builder.CreateAdd(total, term);
  auto* next = builder.CreateAdd(index, builder.getInt64(1));
  index->addIncoming(next, loop);
  total->addIncoming(sum, loop);
  builder.CreateCondBr(builder.CreateICmpSLT(next, builder.getInt64(4)), loop, done);
  builder.SetInsertPoint(done);
  auto* result =
      recursion_in_loop ? sum : builder.CreateAdd(sum, builder.CreateCall(function, {reduced}));
  builder.CreateRet(result);
  ASSERT_FALSE(llvm::verifyModule(module));
  const auto original_count = function->getInstructionCount();
  InlineRecursion(*function);
  EXPECT_FALSE(llvm::verifyModule(module));
  if (recursion_in_loop) {
    EXPECT_EQ(function->getInstructionCount(), original_count);
  } else {
    EXPECT_GT(function->getInstructionCount(), original_count);
  }
  EXPECT_EQ(RecursiveCallCount(*function), 1U);
  EXPECT_EQ(module.size(), 1U);
}

INSTANTIATE_TEST_SUITE_P(CallPlacement, LoopRecursiveInlinerTest, testing::Bool());

}  // namespace
}  // namespace vela::compiler::native
