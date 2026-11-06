#include "compiler/native/recursive_inliner.h"

#include <cstddef>

#include <gtest/gtest.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>

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

}  // namespace
}  // namespace vela::compiler::native
