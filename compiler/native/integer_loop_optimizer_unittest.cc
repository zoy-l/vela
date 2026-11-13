#include "compiler/native/integer_loop_optimizer.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <tuple>

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
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueSymbolTable.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include "base/declaration.h"
#include "compiler/native/memory_regions.h"
#include "compiler/test_support/native_pass_test_support.h"
#include "runtime/native/native_memory_layout.h"

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

enum class GuardMutation : std::uint8_t { None, SameKind, OtherKind, Name, Items, Length, Call };
enum class GuardPath : std::uint8_t {
  Separate,
  Combined,
  Rejoined,
  Conditional,
  EarlyExit,
  RepeatedBound,
};

class GuardedPollOptimizerTest
    : public testing::TestWithParam<std::tuple<GuardMutation, GuardPath>> {};

void TagGuardLoad(llvm::Instruction& instruction) {
  const auto name = instruction.getName();
  auto& context = instruction.getContext();
  if (name == "items") {
    instruction.setMetadata(
        llvm::LLVMContext::MD_tbaa,
        NativeMemoryTag(context, MemoryRegion::FieldPayloads, DeclarationId{1}));
  } else if (name == "kind" || name == "name") {
    instruction.setMetadata(llvm::LLVMContext::MD_tbaa,
                            NativeMemoryTag(context, MemoryRegion::FieldKinds,
                                            DeclarationId{name == "kind" ? 2U : 3U}));
  } else if (name == "length" || name == "data") {
    instruction.setMetadata(llvm::LLVMContext::MD_tbaa,
                            NativeMemoryTag(context, MemoryRegion::HeapHeader));
  } else if (name == "argument") {
    instruction.setMetadata(llvm::LLVMContext::MD_tbaa,
                            NativeMemoryTag(context, MemoryRegion::Elements));
  }
}

TEST_P(GuardedPollOptimizerTest, MovesOnlyGuardsPreservedThroughoutTheSuccessfulChunk) {
  const auto [mutation, path] = GetParam();
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"(
declare void @process()
declare void @mutate()
declare i64 @bind()
define i64 @guarded(ptr %packet, ptr %requests.address, i64 %count, i1 %enabled,
                    ptr %input, ptr %roots) {
entry:
  br label %header
header:
  %index = phi i64 [ 0, %entry ], [ %next.index, %latch ]
  %total = phi i64 [ 0, %entry ], [ %next.total, %latch ]
  %more = icmp slt i64 %index, %count
  br i1 %more, label %arguments, label %done
arguments:
  %items = load ptr, ptr %packet, align 8
  %length.address = getelementptr i8, ptr %items, i64 40
  %length = load i32, ptr %length.address, align 4
  %one = icmp eq i32 %length, 1
  br i1 %one, label %types, label %slow
types:
  %data.address = getelementptr i8, ptr %items, i64 56
  %data = load ptr, ptr %data.address, align 8
  %argument = load ptr, ptr %data, align 8
  %kind.address = getelementptr i8, ptr %argument, i64 72
  %name.address = getelementptr i8, ptr %argument, i64 73
  %kind = load i8, ptr %kind.address, align 1
  %name = load i8, ptr %name.address, align 1
  %integer = icmp eq i8 %kind, 2
  %positional = icmp eq i8 %name, 0
  %valid = and i1 %integer, %positional
  br i1 %valid, label %poll, label %slow
poll:
  %requests = load atomic i8, ptr %requests.address monotonic, align 1, !vela.poll.ready.state !0
  %pending = icmp ne i8 %requests, 0
  br i1 %pending, label %process, label %calculate, !vela.poll.bounded !1
process:
  call void @process()
  br label %calculate
calculate:
  %value.address = getelementptr i8, ptr %argument, i64 56
  %value = load i64, ptr %value.address, align 8
  %fast.total = xor i64 %total, %value
  br label %latch
slow:
  %bound = call i64 @bind(), !vela.guard.failure !1
  %slow.total = xor i64 %total, %bound
  br label %latch
latch:
  %next.total = phi i64 [ %fast.total, %calculate ], [ %slow.total, %slow ]
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
  auto& function = *module->getFunction("guarded");
  const auto& symbols = *function.getValueSymbolTable();
  auto* kind = symbols.lookup("kind.address");
  auto* name = symbols.lookup("name.address");
  auto* length = symbols.lookup("length.address");
  auto* calculate = llvm::dyn_cast_or_null<llvm::BasicBlock>(symbols.lookup("calculate"));
  auto* arguments = llvm::dyn_cast_or_null<llvm::BasicBlock>(symbols.lookup("arguments"));
  auto* types = llvm::dyn_cast_or_null<llvm::BasicBlock>(symbols.lookup("types"));
  auto* poll = llvm::dyn_cast_or_null<llvm::BasicBlock>(symbols.lookup("poll"));
  auto* slow = llvm::dyn_cast_or_null<llvm::BasicBlock>(symbols.lookup("slow"));
  auto* requests = llvm::dyn_cast_or_null<llvm::LoadInst>(symbols.lookup("requests"));
  for (auto& instruction : llvm::instructions(function)) {
    TagGuardLoad(instruction);
  }
  ASSERT_NE(calculate, nullptr);
  ASSERT_NE(arguments, nullptr);
  ASSERT_NE(types, nullptr);
  ASSERT_NE(poll, nullptr);
  ASSERT_NE(slow, nullptr);
  ASSERT_NE(requests, nullptr);
  if (path == GuardPath::RepeatedBound) {
    auto* header = arguments->getSinglePredecessor();
    auto* more = llvm::cast<llvm::ICmpInst>(symbols.lookup("more"));
    llvm::IRBuilder<> condition(more);
    auto* bound_address = condition.CreateConstGEP1_64(
        condition.getInt8Ty(), symbols.lookup("input"), runtime::NativeMemoryLayout::kObjectLength);
    auto* bound = condition.CreateLoad(condition.getInt32Ty(), bound_address);
    auto* tag = NativeMemoryTag(context, MemoryRegion::HeapHeader);
    bound->setMetadata(llvm::LLVMContext::MD_tbaa, tag);
    more->setOperand(1, condition.CreateZExt(bound, condition.getInt64Ty()));
    auto* bounds = llvm::BasicBlock::Create(context, "bounds", &function, arguments);
    auto* invalid = llvm::BasicBlock::Create(context, "invalid", &function);
    header->getTerminator()->replaceSuccessorWith(arguments, bounds);
    llvm::IRBuilder<> checking(bounds);
    auto* clear = checking.CreateStore(checking.getInt8(0), symbols.lookup("roots"));
    clear->setMetadata(llvm::LLVMContext::MD_tbaa, NativeMemoryTag(context, MemoryRegion::Roots));
    clear->setMetadata(kNativeRootClearMetadata, llvm::MDNode::get(context, {}));
    auto* reloaded = checking.CreateLoad(checking.getInt32Ty(), bound_address);
    reloaded->setMetadata(llvm::LLVMContext::MD_tbaa, tag);
    auto* valid = checking.CreateICmpSLT(&*header->phis().begin(),
                                         checking.CreateZExt(reloaded, checking.getInt64Ty()));
    checking.CreateCondBr(valid, arguments, invalid);
    llvm::IRBuilder<>(invalid).CreateRet(
        condition.getInt64(std::numeric_limits<std::uint64_t>::max()));
  }
  if (path == GuardPath::Combined || path == GuardPath::Rejoined) {
    auto* available = llvm::cast<llvm::CondBrInst>(arguments->getTerminator());
    auto* polling = llvm::cast<llvm::CondBrInst>(poll->getTerminator());
    requests->moveBefore(available->getIterator());
    llvm::IRBuilder<> entry(available);
    auto* ready = entry.CreateICmpEQ(requests, entry.getInt8(0));
    auto* one = available->getCondition();
    available->setCondition(entry.CreateAnd(one, ready));
    available->setMetadata(kNativeBoundedPollMetadata,
                           polling->getMetadata(kNativeBoundedPollMetadata));
    llvm::UncondBrInst::Create(calculate, polling->getIterator());
    polling->eraseFromParent();
    if (path == GuardPath::Rejoined) {
      auto* resolve = llvm::BasicBlock::Create(context, "resolve", &function);
      llvm::IRBuilder<> resolving(resolve);
      auto* call = resolving.CreateCall(module->getFunction("process"));
      call->setMetadata(kNativeGuardFailureMetadata,
                        slow->front().getMetadata(kNativeGuardFailureMetadata));
      resolving.CreateBr(types);
      auto* cached = llvm::BasicBlock::Create(context, "cached", &function);
      llvm::IRBuilder<> caching(cached);
      auto* address =
          caching.CreateConstGEP1_64(caching.getInt8Ty(), function.getArg(0), sizeof(void*));
      auto* flag = caching.CreateLoad(caching.getInt8Ty(), address);
      flag->setMetadata(llvm::LLVMContext::MD_tbaa,
                        NativeMemoryTag(context, MemoryRegion::ReflectionCache));
      caching.CreateCondBr(caching.CreateICmpEQ(flag, caching.getInt8(1)), types, resolve);
      available->setSuccessor(0, cached);
      available->setSuccessor(1, resolve);
    }
  }
  if (path == GuardPath::Conditional || path == GuardPath::EarlyExit) {
    auto* header = arguments->getSinglePredecessor();
    auto* selected = llvm::BasicBlock::Create(context, "selected", &function);
    auto* skipped = llvm::BasicBlock::Create(context, "skipped", &function);
    header->getTerminator()->replaceSuccessorWith(arguments, selected);
    llvm::IRBuilder<>(selected).CreateCondBr(function.getArg(3), arguments, skipped);
    llvm::IRBuilder<> skipping(skipped);
    auto* total = &*std::next(header->phis().begin());
    if (path == GuardPath::Conditional) {
      auto* latch = calculate->getSingleSuccessor();
      skipping.CreateBr(latch);
      latch->phis().begin()->addIncoming(total, skipped);
    } else {
      skipping.CreateRet(total);
    }
  }
  llvm::IRBuilder<> builder(&*calculate->getFirstInsertionPt());
  llvm::StoreInst* store = nullptr;
  switch (mutation) {
    case GuardMutation::None:
      break;
    case GuardMutation::SameKind:
    case GuardMutation::OtherKind:
      store =
          builder.CreateStore(builder.getInt8(mutation == GuardMutation::SameKind ? 2 : 3), kind);
      store->setMetadata(llvm::LLVMContext::MD_tbaa,
                         NativeMemoryTag(context, MemoryRegion::FieldKinds, DeclarationId{2}));
      break;
    case GuardMutation::Name:
      store = builder.CreateStore(builder.getInt8(4), name);
      store->setMetadata(llvm::LLVMContext::MD_tbaa,
                         NativeMemoryTag(context, MemoryRegion::FieldKinds, DeclarationId{3}));
      break;
    case GuardMutation::Items:
      store = builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()),
                                  function.getArg(0));
      store->setMetadata(llvm::LLVMContext::MD_tbaa,
                         NativeMemoryTag(context, MemoryRegion::FieldPayloads, DeclarationId{1}));
      break;
    case GuardMutation::Length:
      store = builder.CreateStore(builder.getInt32(0), length);
      store->setMetadata(llvm::LLVMContext::MD_tbaa,
                         NativeMemoryTag(context, MemoryRegion::HeapHeader));
      break;
    case GuardMutation::Call:
      builder.CreateCall(module->getFunction("mutate"));
      break;
  }
  ASSERT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  test::RunFunctionPasses(function, AddBoundedPollPasses);
  EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  llvm::DominatorTree dominators(function);
  const llvm::LoopInfo loops(dominators);
  bool unpolled = false;
  for (const auto* loop : loops.getLoopsInPreorder()) {
    unpolled = unpolled || (loop->getSubLoops().empty() && HasUnpolledArrayLoad(*loop));
  }
  std::string generated;
  llvm::raw_string_ostream stream(generated);
  function.print(stream);
  const auto always_executed = path != GuardPath::Conditional && path != GuardPath::EarlyExit;
  EXPECT_EQ(unpolled, always_executed &&
                          (mutation == GuardMutation::None || mutation == GuardMutation::SameKind))
      << generated;
}

INSTANTIATE_TEST_SUITE_P(GuardWrites,
                         GuardedPollOptimizerTest,
                         testing::Combine(testing::Values(GuardMutation::None,
                                                          GuardMutation::SameKind,
                                                          GuardMutation::OtherKind,
                                                          GuardMutation::Name,
                                                          GuardMutation::Items,
                                                          GuardMutation::Length,
                                                          GuardMutation::Call),
                                          testing::Values(GuardPath::Separate,
                                                          GuardPath::Combined,
                                                          GuardPath::Rejoined,
                                                          GuardPath::Conditional,
                                                          GuardPath::EarlyExit,
                                                          GuardPath::RepeatedBound)));

}  // namespace
}  // namespace vela::compiler::native
