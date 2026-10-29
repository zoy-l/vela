#include "compiler/native/integer_loop_optimizer.h"

#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

#include "runtime/native_memory_layout.h"

namespace vela::compiler::native {
namespace {

struct CheckedUpdate {
  llvm::IntrinsicInst* call;
  llvm::ExtractValueInst* value;
  llvm::ExtractValueInst* overflow;
  llvm::BranchInst* branch;
  llvm::BasicBlock* failed;
};

std::optional<CheckedUpdate> MatchUpdate(llvm::Value* incoming, const llvm::Loop& loop) {
  auto* value = llvm::dyn_cast<llvm::ExtractValueInst>(incoming);
  if (value == nullptr || value->getNumIndices() != 1 || value->getIndices().front() != 0) {
    return std::nullopt;
  }
  auto* call = llvm::dyn_cast<llvm::IntrinsicInst>(value->getAggregateOperand());
  if (call == nullptr || !loop.contains(call)) {
    return std::nullopt;
  }
  const auto intrinsic = call->getIntrinsicID();
  if (intrinsic != llvm::Intrinsic::sadd_with_overflow &&
      intrinsic != llvm::Intrinsic::ssub_with_overflow) {
    return std::nullopt;
  }
  llvm::ExtractValueInst* overflow = nullptr;
  for (auto* user : call->users()) {
    if (user == value) {
      continue;
    }
    auto* extract = llvm::dyn_cast<llvm::ExtractValueInst>(user);
    if (extract == nullptr || overflow != nullptr || extract->getNumIndices() != 1 ||
        extract->getIndices().front() != 1) {
      return std::nullopt;
    }
    overflow = extract;
  }
  if (overflow == nullptr || !overflow->hasOneUse()) {
    return std::nullopt;
  }
  auto* branch = llvm::dyn_cast<llvm::BranchInst>(call->getParent()->getTerminator());
  if (branch == nullptr || !branch->isConditional()) {
    return std::nullopt;
  }
  auto* condition = branch->getCondition();
  unsigned failure_edge = 0;
  if (condition != overflow) {
    if (!llvm::PatternMatch::match(
            condition, llvm::PatternMatch::m_Not(llvm::PatternMatch::m_Specific(overflow)))) {
      return std::nullopt;
    }
    failure_edge = 1;
  }
  auto* failed = branch->getSuccessor(failure_edge);
  if (loop.contains(failed) || !loop.contains(branch->getSuccessor(1 - failure_edge))) {
    return std::nullopt;
  }
  return CheckedUpdate{call, value, overflow, branch, failed};
}

struct CountedLoop {
  llvm::Loop* loop;
  llvm::PHINode* index;
  llvm::Value* bound;
  llvm::ICmpInst* comparison;
  llvm::BranchInst* condition;
  unsigned exit_edge;
  CheckedUpdate step;
  int direction;
};

std::optional<CountedLoop> MatchCountedLoop(llvm::Loop& loop, llvm::DominatorTree& dominators) {
  if (!loop.isLoopSimplifyForm()) {
    return std::nullopt;
  }
  llvm::SmallVector<llvm::BasicBlock*> exits;
  loop.getExitingBlocks(exits);
  for (auto* block : exits) {
    auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (branch == nullptr || !branch->isConditional()) {
      continue;
    }
    auto* condition = branch->getCondition();
    llvm::Value* unwrapped = nullptr;
    const bool inverted = llvm::PatternMatch::match(
        condition, llvm::PatternMatch::m_Not(llvm::PatternMatch::m_Value(unwrapped)));
    auto* comparison = llvm::dyn_cast<llvm::ICmpInst>(inverted ? unwrapped : condition);
    if (comparison == nullptr) {
      continue;
    }
    const unsigned exit_edge = loop.contains(branch->getSuccessor(0)) ? 1 : 0;
    auto predicate = comparison->getPredicate();
    if (inverted == (exit_edge == 1)) {
      predicate = llvm::CmpInst::getInversePredicate(predicate);
    }
    auto* index = llvm::dyn_cast<llvm::PHINode>(comparison->getOperand(0));
    auto* bound = comparison->getOperand(1);
    if (index == nullptr || index->getParent() != loop.getHeader()) {
      index = llvm::dyn_cast<llvm::PHINode>(bound);
      bound = comparison->getOperand(0);
      predicate = llvm::CmpInst::getSwappedPredicate(predicate);
    }
    if (index == nullptr || index->getParent() != loop.getHeader() ||
        !index->getType()->isIntegerTy(64) || !loop.isLoopInvariant(bound)) {
      continue;
    }
    const auto step = MatchUpdate(index->getIncomingValueForBlock(loop.getLoopLatch()), loop);
    if (!step || !dominators.dominates(block, step->call->getParent())) {
      continue;
    }
    auto* left = step->call->getArgOperand(0);
    auto* right = step->call->getArgOperand(1);
    const auto intrinsic = step->call->getIntrinsicID();
    if (right == index && intrinsic == llvm::Intrinsic::sadd_with_overflow) {
      std::swap(left, right);
    }
    auto* constant = llvm::dyn_cast<llvm::ConstantInt>(right);
    if (left != index || constant == nullptr || !constant->isOne()) {
      continue;
    }
    const int direction = intrinsic == llvm::Intrinsic::sadd_with_overflow ? 1 : -1;
    if ((direction == 1 && predicate == llvm::CmpInst::ICMP_SLT) ||
        (direction == -1 && predicate == llvm::CmpInst::ICMP_SGT)) {
      return CountedLoop{&loop, index, bound, comparison, branch, exit_edge, *step, direction};
    }
  }
  return std::nullopt;
}

struct Reduction {
  llvm::PHINode* accumulator;
  llvm::Value* term;
  CheckedUpdate update;
};

void AddUpdateInstructions(llvm::SmallPtrSetImpl<llvm::Value*>& allowed,
                           const CheckedUpdate& update) {
  allowed.insert(update.call);
  allowed.insert(update.value);
  allowed.insert(update.overflow);
  allowed.insert(update.branch->getCondition());
}

std::optional<Reduction> MatchReduction(const CountedLoop& counted,
                                        llvm::ScalarEvolution& evolution,
                                        llvm::DominatorTree& dominators) {
  const auto& loop = *counted.loop;
  if (!loop.getSubLoops().empty()) {
    return std::nullopt;
  }
  llvm::PHINode* accumulator = nullptr;
  for (auto& phi : loop.getHeader()->phis()) {
    if (&phi == counted.index) {
      continue;
    }
    if (accumulator != nullptr || !phi.getType()->isIntegerTy(64)) {
      return std::nullopt;
    }
    accumulator = &phi;
  }
  if (accumulator == nullptr) {
    return std::nullopt;
  }
  const auto update = MatchUpdate(accumulator->getIncomingValueForBlock(loop.getLoopLatch()), loop);
  if (!update || !dominators.dominates(counted.condition->getParent(), update->call->getParent()) ||
      !update->failed->phis().empty()) {
    return std::nullopt;
  }
  auto* left = update->call->getArgOperand(0);
  auto* term = update->call->getArgOperand(1);
  if (term == accumulator &&
      update->call->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow) {
    std::swap(left, term);
  }
  if (left != accumulator) {
    return std::nullopt;
  }
  if (term == counted.index) {
    auto* start = counted.index->getIncomingValueForBlock(loop.getLoopPreheader());
    const auto* start_range = evolution.getSCEV(start);
    const auto* bound_range = evolution.getSCEV(counted.bound);
    // 与 Go loopbce、Dart RangeAnalysis 一样，先用循环条件约束 induction range。
    // 跨零的等差项可能让中间前缀先溢出再回到范围内，不能只检查最终值。
    const bool monotone = counted.direction == 1 ? evolution.isKnownNonNegative(start_range) ||
                                                       evolution.isKnownNonPositive(bound_range)
                                                 : evolution.isKnownNonPositive(start_range) ||
                                                       evolution.isKnownNonNegative(bound_range);
    if (!monotone) {
      return std::nullopt;
    }
  } else if (!loop.isLoopInvariant(term)) {
    return std::nullopt;
  }

  llvm::SmallPtrSet<llvm::Value*, 16> allowed;
  allowed.insert(counted.index);
  allowed.insert(accumulator);
  allowed.insert(counted.comparison);
  allowed.insert(counted.condition->getCondition());
  AddUpdateInstructions(allowed, counted.step);
  AddUpdateInstructions(allowed, *update);
  auto* normal_exit = counted.condition->getSuccessor(counted.exit_edge);
  for (auto* block : loop.blocks()) {
    for (auto& instruction : *block) {
      if (auto* branch = llvm::dyn_cast<llvm::BranchInst>(&instruction)) {
        if (branch->isConditional() && branch != counted.condition &&
            branch != counted.step.branch && branch != update->branch) {
          return std::nullopt;
        }
      } else if (!allowed.contains(&instruction)) {
        return std::nullopt;
      }
      for (auto* user : instruction.users()) {
        auto* use = llvm::cast<llvm::Instruction>(user);
        if (!loop.contains(use) &&
            (use->getParent() != normal_exit ||
             (&instruction != accumulator && &instruction != counted.index))) {
          return std::nullopt;
        }
      }
    }
  }
  // 累加器只能参与自己的 recurrence；比较、再次使用等都会观察中间值。
  for (auto* user : accumulator->users()) {
    if (loop.contains(llvm::cast<llvm::Instruction>(user)) && user != update->call) {
      return std::nullopt;
    }
  }
  return Reduction{accumulator, term, *update};
}

void ReplaceUpdate(const CheckedUpdate& update, llvm::Value* result) {
  update.value->replaceAllUsesWith(result);
  update.overflow->replaceAllUsesWith(llvm::ConstantInt::getFalse(update.call->getContext()));
  update.value->eraseFromParent();
  update.overflow->eraseFromParent();
  update.call->eraseFromParent();
}

void WidenReduction(const CountedLoop& counted, const Reduction& reduction) {
  auto& loop = *counted.loop;
  auto* accumulator = reduction.accumulator;
  auto* header = loop.getHeader();
  auto* preheader = loop.getLoopPreheader();
  llvm::IRBuilder<> entry(preheader->getTerminator());
  auto* wide_type = entry.getInt128Ty();
  auto* initial = entry.CreateSExt(accumulator->getIncomingValueForBlock(preheader), wide_type);
  auto* wide = llvm::PHINode::Create(wide_type, 2, "sum.wide", accumulator->getIterator());
  wide->addIncoming(initial, preheader);
  llvm::IRBuilder<> body(reduction.update.call);
  auto* term = body.CreateSExt(reduction.term, wide_type);
  // 严格比较、单位步长最多执行 2^64-1 次。任意 int64 初值和同号 int64
  // 加/减项的所有前缀都能以有符号 i128 精确表示，NSW 不依赖最终 i64 检查。
  auto* next = reduction.update.call->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow
                   ? body.CreateNSWAdd(wide, term, "sum.next")
                   : body.CreateNSWSub(wide, term, "sum.next");
  wide->addIncoming(next, loop.getLoopLatch());
  ReplaceUpdate(reduction.update, body.CreateTrunc(next, body.getInt64Ty()));
  llvm::IRBuilder<> header_builder(header, header->getFirstInsertionPt());
  auto* narrow = header_builder.CreateTrunc(wide, header_builder.getInt64Ty(), "sum.result");
  accumulator->replaceAllUsesWith(narrow);
  accumulator->eraseFromParent();

  auto* condition = counted.condition;
  auto* normal_exit = condition->getSuccessor(counted.exit_edge);
  auto* check =
      llvm::BasicBlock::Create(header->getContext(), "sum.range", header->getParent(), normal_exit);
  condition->setSuccessor(counted.exit_edge, check);
  normal_exit->replacePhiUsesWith(condition->getParent(), check);
  llvm::IRBuilder<> exit(check);
  auto* fits = exit.CreateICmpEQ(wide, exit.CreateSExt(narrow, wide_type));
  // 重用原失败块，保留错误类型、源位置、保根与 catch/finally 路径。
  // MatchReduction 已排除所有能在失败路径观察中间迭代状态的 SSA uses。
  exit.CreateCondBr(fits, normal_exit, reduction.update.failed);
}

struct FieldReduction {
  llvm::LoadInst* load;
  llvm::StoreInst* store;
  CheckedUpdate update;
};

std::optional<FieldReduction> MatchFieldReduction(const CountedLoop& counted,
                                                  llvm::DominatorTree& dominators) {
  const auto& loop = *counted.loop;
  if (!loop.getSubLoops().empty()) {
    return std::nullopt;
  }
  llvm::LoadInst* load = nullptr;
  llvm::StoreInst* store = nullptr;
  llvm::StoreInst* tag = nullptr;
  for (auto* block : loop.blocks()) {
    for (auto& instruction : *block) {
      if (auto* read = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (load != nullptr || !read->isSimple() || !read->getType()->isIntegerTy(64)) {
          return std::nullopt;
        }
        load = read;
      }
      if (auto* write = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        if (!write->isSimple()) {
          return std::nullopt;
        }
        if (write->getMetadata(kNativeRootClearMetadata) != nullptr) {
          continue;
        }
        if (write->getValueOperand()->getType()->isIntegerTy(64) && store == nullptr) {
          store = write;
        } else if (auto* kind = llvm::dyn_cast<llvm::ConstantInt>(write->getValueOperand());
                   kind != nullptr && kind->getType()->isIntegerTy(8) &&
                   kind->getZExtValue() == static_cast<std::uint8_t>(runtime::ValueKind::Integer) &&
                   tag == nullptr) {
          tag = write;
        } else {
          return std::nullopt;
        }
      }
    }
  }
  if (load == nullptr || store == nullptr || tag == nullptr ||
      load->getPointerOperand() != store->getPointerOperand()) {
    return std::nullopt;
  }
  const auto update = MatchUpdate(store->getValueOperand(), loop);
  if (!update || !update->failed->phis().empty() ||
      !dominators.dominates(store, counted.step.call) ||
      !dominators.dominates(counted.condition->getParent(), load->getParent())) {
    return std::nullopt;
  }
  auto* term = llvm::dyn_cast<llvm::ConstantInt>(update->call->getArgOperand(1));
  if (update->call->getArgOperand(0) != load || term == nullptr || !term->isOne()) {
    return std::nullopt;
  }
  const auto& layout = load->getModule()->getDataLayout();
  std::int64_t value_offset = 0;
  std::int64_t tag_offset = 0;
  auto* base =
      llvm::GetPointerBaseWithConstantOffset(load->getPointerOperand(), value_offset, layout);
  auto* tag_base =
      llvm::GetPointerBaseWithConstantOffset(tag->getPointerOperand(), tag_offset, layout);
  if (!loop.isLoopInvariant(base) || tag_base != base ||
      tag_offset - value_offset !=
          static_cast<std::int64_t>(runtime::NativeMemoryLayout::kValueKind)) {
    return std::nullopt;
  }
  llvm::SmallPtrSet<llvm::Value*, 16> allowed;
  allowed.insert(load);
  allowed.insert(store);
  allowed.insert(tag);
  allowed.insert(counted.index);
  allowed.insert(counted.comparison);
  allowed.insert(counted.condition->getCondition());
  AddUpdateInstructions(allowed, counted.step);
  AddUpdateInstructions(allowed, *update);
  for (auto* block : loop.blocks()) {
    for (auto& instruction : *block) {
      if (instruction.getMetadata(kNativeRootClearMetadata) != nullptr) {
        continue;
      }
      if (auto* branch = llvm::dyn_cast<llvm::BranchInst>(&instruction)) {
        if (branch->isConditional() && branch != counted.condition &&
            branch != counted.step.branch && branch != update->branch &&
            !llvm::isa<llvm::ConstantInt>(branch->getCondition())) {
          auto* check = llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
          // 保留第一次迭代的空引用检查；零次迭代不访问对象。
          if (check == nullptr || !check->isEquality() || check->getOperand(0) != base ||
              !llvm::isa<llvm::ConstantPointerNull>(check->getOperand(1))) {
            return std::nullopt;
          }
          allowed.insert(check);
        }
        continue;
      }
      if (!allowed.contains(&instruction) && !llvm::isa<llvm::GetElementPtrInst>(instruction) &&
          !llvm::isa<llvm::ICmpInst>(instruction)) {
        return std::nullopt;
      }
      for (auto* user : instruction.users()) {
        if (!loop.contains(llvm::cast<llvm::Instruction>(user)) &&
            !llvm::isa<llvm::GetElementPtrInst>(instruction)) {
          // 循环索引或临时值若在 catch 等路径可见，不能跳过其中间状态。
          return std::nullopt;
        }
      }
    }
  }
  return FieldReduction{load, store, *update};
}

void FoldFieldReduction(const CountedLoop& counted, const FieldReduction& field) {
  auto* call = field.update.call;
  llvm::IRBuilder<> builder(call);
  auto* wide = builder.getInt128Ty();
  auto* index = builder.CreateSExt(counted.index, wide);
  auto* bound = builder.CreateSExt(counted.bound, wide);
  auto* count = counted.direction == 1 ? builder.CreateNSWSub(bound, index)
                                       : builder.CreateNSWSub(index, bound);
  const bool adding = call->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow;
  auto* initial = builder.CreateSExt(field.load, wide);
  auto* final =
      adding ? builder.CreateNSWAdd(initial, count) : builder.CreateNSWSub(initial, count);
  auto* result = builder.CreateTrunc(final, builder.getInt64Ty());
  auto* overflow = builder.CreateICmpNE(final, builder.CreateSExt(result, wide));
  field.update.value->replaceAllUsesWith(result);
  field.update.overflow->replaceAllUsesWith(overflow);
  field.update.value->eraseFromParent();
  field.update.overflow->eraseFromParent();
  call->eraseFromParent();
  // 单位增减在溢出前必然到达 int64 边界。失败时先写回最后一次成功的字段值，
  // 再进入原错误块，保留对象别名、catch/finally 与源位置可观察的前缀。
  auto* failed = field.update.failed;
  auto* prefix =
      llvm::BasicBlock::Create(failed->getContext(), "field.prefix", failed->getParent(), failed);
  llvm::IRBuilder<> failure(prefix);
  failure.CreateStore(failure.getInt64(std::bit_cast<std::uint64_t>(
                          adding ? std::numeric_limits<std::int64_t>::max()
                                 : std::numeric_limits<std::int64_t>::min())),
                      field.store->getPointerOperand());
  failure.CreateBr(failed);
  field.update.branch->replaceSuccessorWith(failed, prefix);
  ReplaceUpdate(counted.step, counted.bound);
}

class FieldLoopPass : public llvm::PassInfoMixin<FieldLoopPass> {
 public:
  llvm::PreservedAnalyses run(llvm::Function& function, llvm::FunctionAnalysisManager& analyses) {
    auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    bool changed = false;
    for (auto* loop : loops.getLoopsInPreorder()) {
      const auto counted = MatchCountedLoop(*loop, dominators);
      if (!counted) {
        continue;
      }
      const auto field = MatchFieldReduction(*counted, dominators);
      if (field) {
        FoldFieldReduction(*counted, *field);
        changed = true;
      }
    }
    return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
  }
};

class IntegerLoopPass : public llvm::PassInfoMixin<IntegerLoopPass> {
 public:
  llvm::PreservedAnalyses run(llvm::Function& function, llvm::FunctionAnalysisManager& analyses) {
    auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    auto& evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
    struct Plan {
      CountedLoop counted;
      std::optional<Reduction> reduction;
    };
    llvm::SmallVector<Plan> plans;
    for (auto* loop : loops.getLoopsInPreorder()) {
      const auto counted = MatchCountedLoop(*loop, dominators);
      if (counted) {
        plans.push_back({*counted, MatchReduction(*counted, evolution, dominators)});
      }
    }
    for (const auto& plan : plans) {
      const auto& counted = plan.counted;
      if (plan.reduction) {
        WidenReduction(counted, *plan.reduction);
      }
      const auto& step = counted.step;
      llvm::IRBuilder<> builder(step.call);
      // index < bound 保证 index + 1 <= INT64_MAX；递减的严格比较同理。
      auto* next = counted.direction == 1
                       ? builder.CreateNSWAdd(counted.index, builder.getInt64(1))
                       : builder.CreateNSWSub(counted.index, builder.getInt64(1));
      ReplaceUpdate(step, next);
    }
    return plans.empty() ? llvm::PreservedAnalyses::all() : llvm::PreservedAnalyses::none();
  }
};

}  // namespace

void AddIntegerLoopPasses(llvm::FunctionPassManager& passes) {
  passes.addPass(llvm::PromotePass());
  passes.addPass(llvm::LoopSimplifyPass());
  passes.addPass(llvm::EarlyCSEPass());
  passes.addPass(llvm::DCEPass());
  passes.addPass(FieldLoopPass());
  passes.addPass(IntegerLoopPass());
}

}  // namespace vela::compiler::native
