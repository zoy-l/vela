#include "compiler/native/integer_loop_optimizer.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/Support/AtomicOrdering.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ModRef.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/CorrelatedValuePropagation.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/JumpThreading.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/LCSSA.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "compiler/native/function_emitter.h"
#include "compiler/native/local_ssa.h"
#include "compiler/native/loop_cloning.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {
namespace {

constexpr auto kIntegerBits = std::numeric_limits<std::uint64_t>::digits;
constexpr auto kValueKindBits = std::numeric_limits<std::uint8_t>::digits;
// 常见的归约只有两组受查更新，内联容纳其指令集合。
constexpr unsigned kInlineAllowedValues = 16;
using AllowedInstructions = llvm::SmallPtrSet<llvm::Value*, kInlineAllowedValues>;
constexpr unsigned kInlinePollBlocks = 16;
constexpr unsigned kInlineGuardValues = 32;

struct CheckedUpdate {
  llvm::IntrinsicInst* call;
  llvm::ExtractValueInst* value;
  llvm::ExtractValueInst* overflow;
  llvm::CondBrInst* branch;
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
  auto* branch = llvm::dyn_cast<llvm::CondBrInst>(call->getParent()->getTerminator());
  if (branch == nullptr) {
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
  return CheckedUpdate{
      .call = call,
      .value = value,
      .overflow = overflow,
      .branch = branch,
      .failed = failed,
  };
}

struct CountedLoop {
  llvm::Loop* loop;
  llvm::PHINode* index;
  llvm::Value* bound;
  llvm::ICmpInst* comparison;
  llvm::CondBrInst* condition;
  unsigned exit_edge;
  llvm::Instruction* increment;
  std::optional<CheckedUpdate> step;
  int direction;
};

std::optional<int> MatchStepDirection(llvm::PHINode& index, llvm::Instruction& increment) {
  auto* left = increment.getOperand(0);
  auto* right = increment.getOperand(1);
  const auto* checked = llvm::dyn_cast<llvm::IntrinsicInst>(&increment);
  const bool adding = checked != nullptr
                          ? checked->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow
                          : increment.getOpcode() == llvm::Instruction::Add;
  if (right == &index && adding) {
    std::swap(left, right);
  }
  const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(right);
  if (left != &index || constant == nullptr || (!constant->isOne() && !constant->isMinusOne())) {
    return std::nullopt;
  }
  return (adding ? 1 : -1) * (constant->isOne() ? 1 : -1);
}

std::optional<CountedLoop> MatchLoopExit(llvm::Loop& loop,
                                         llvm::BasicBlock* block,
                                         llvm::DominatorTree& dominators,
                                         const llvm::SmallPtrSetImpl<llvm::Value*>* stable) {
  auto* branch = llvm::dyn_cast<llvm::CondBrInst>(block->getTerminator());
  if (branch == nullptr) {
    return std::nullopt;
  }
  auto* condition = branch->getCondition();
  llvm::Value* unwrapped = nullptr;
  const bool inverted = llvm::PatternMatch::match(
      condition, llvm::PatternMatch::m_Not(llvm::PatternMatch::m_Value(unwrapped)));
  auto* comparison = llvm::dyn_cast<llvm::ICmpInst>(inverted ? unwrapped : condition);
  if (comparison == nullptr) {
    return std::nullopt;
  }
  const unsigned exit_edge = loop.contains(branch->getSuccessor(0)) ? 1 : 0;
  // LLVM 可将已证明同号的有符号比较改为 unsigned；保留其范围证明。
  auto predicate =
      comparison->hasSameSign() ? comparison->getSignedPredicate() : comparison->getPredicate();
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
      !index->getType()->isIntegerTy(kIntegerBits) ||
      (!loop.isLoopInvariant(bound) && (stable == nullptr || !stable->contains(bound)))) {
    return std::nullopt;
  }
  auto* incoming = index->getIncomingValueForBlock(loop.getLoopLatch());
  const auto step = MatchUpdate(incoming, loop);
  auto* plain = llvm::dyn_cast<llvm::BinaryOperator>(incoming);
  // 前一轮范围分析或 LLVM 已将受查步进化简为 NSW。保留这一证明，使内联后
  // 才出现的归约和有界安全点仍可被后续 pass 识别；普通回绕加减不能作此推导。
  if (!step && (plain == nullptr || !plain->hasNoSignedWrap() ||
                (plain->getOpcode() != llvm::Instruction::Add &&
                 plain->getOpcode() != llvm::Instruction::Sub))) {
    return std::nullopt;
  }
  llvm::Instruction* increment = step ? step->call : static_cast<llvm::Instruction*>(plain);
  if (!loop.contains(increment) || !dominators.dominates(block, increment->getParent())) {
    return std::nullopt;
  }
  const auto direction = MatchStepDirection(*index, *increment);
  if (!direction) {
    return std::nullopt;
  }
  if ((*direction == 1 && predicate == llvm::CmpInst::ICMP_SLT) ||
      (*direction == -1 && predicate == llvm::CmpInst::ICMP_SGT)) {
    return CountedLoop{
        .loop = &loop,
        .index = index,
        .bound = bound,
        .comparison = comparison,
        .condition = branch,
        .exit_edge = exit_edge,
        .increment = increment,
        .step = step,
        .direction = *direction,
    };
  }
  return std::nullopt;
}

std::optional<CountedLoop> MatchCountedLoop(
    llvm::Loop& loop,
    llvm::DominatorTree& dominators,
    const llvm::SmallPtrSetImpl<llvm::Value*>* stable = nullptr) {
  if (!loop.isLoopSimplifyForm()) {
    return std::nullopt;
  }
  llvm::SmallVector<llvm::BasicBlock*> exits;
  loop.getExitingBlocks(exits);
  for (auto* block : exits) {
    const auto counted = MatchLoopExit(loop, block, dominators, stable);
    if (counted) {
      return counted;
    }
  }
  return std::nullopt;
}

// 纯计算内核最多延迟一个有界片段的停止请求。入口保留请求检查；包含宿主、分配、
// 非纯脚本调用或调试逐行观测的循环仍执行原安全点。限制同时覆盖迭代数与代码体积。
constexpr std::uint64_t kMaximumUnpolledIterations = 1024;
constexpr std::uint64_t kMaximumUnpolledInstructions = 131072;

struct BoundedPoll final {
  llvm::CondBrInst* branch;
  unsigned ready_edge;
  bool guarded_poll = false;
};

struct BoundedRegion final {
  std::vector<BoundedPoll> polls;
  std::vector<BoundedPoll> guards;
  llvm::SmallVector<llvm::BasicBlock*, kInlinePollBlocks> blocks;
  std::uint64_t instructions = 0;
};

llvm::Constant* PollReadyValue(llvm::Value* value, const llvm::DataLayout& layout) {
  if (auto* constant = llvm::dyn_cast<llvm::Constant>(value)) {
    return constant;
  }
  auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
  if (instruction == nullptr) {
    return nullptr;
  }
  if (const auto* state = instruction->getMetadata(kNativePollReadyStateMetadata)) {
    return llvm::mdconst::extract<llvm::Constant>(state->getOperand(0));
  }
  if (!llvm::isa<llvm::CastInst, llvm::BinaryOperator, llvm::ICmpInst>(instruction)) {
    return nullptr;
  }
  llvm::SmallVector<llvm::Constant*, 2> operands;
  for (auto& operand : instruction->operands()) {
    auto* ready = PollReadyValue(operand, layout);
    if (ready == nullptr) {
      return nullptr;
    }
    operands.push_back(ready);
  }
  return llvm::ConstantFoldInstOperands(instruction, operands, layout);
}

llvm::SmallPtrSet<llvm::BasicBlock*, kInlinePollBlocks> FindGuardFailures(const llvm::Loop& loop) {
  llvm::SmallPtrSet<llvm::BasicBlock*, kInlinePollBlocks> failures;
  llvm::SmallVector<llvm::BasicBlock*, kInlinePollBlocks> cold;
  llvm::DenseMap<llvm::BasicBlock*, unsigned> remaining;
  for (auto* block : loop.blocks()) {
    remaining.try_emplace(block, block->getTerminator()->getNumSuccessors());
    if (std::ranges::any_of(*block, [](const auto& instruction) {
          return llvm::isa<llvm::CallBase>(instruction) &&
                 instruction.getMetadata(kNativeGuardFailureMetadata) != nullptr;
        })) {
      failures.insert(block);
      cold.push_back(block);
    }
  }
  while (!cold.empty()) {
    auto* block = cold.pop_back_val();
    for (auto* predecessor : llvm::predecessors(block)) {
      if (loop.contains(predecessor) && !failures.contains(predecessor)) {
        auto& successors = remaining.find(predecessor)->second;
        if (--successors == 0) {
          failures.insert(predecessor);
          cold.push_back(predecessor);
        }
      }
    }
  }
  return failures;
}

BoundedRegion FindBoundedRegion(const llvm::Loop& loop) {
  BoundedRegion region;
  if (!loop.getSubLoops().empty()) {
    return region;
  }
  const auto failures = FindGuardFailures(loop);
  llvm::SmallVector<llvm::BasicBlock*, kInlinePollBlocks> pending{loop.getHeader()};
  llvm::SmallPtrSet<llvm::BasicBlock*, kInlinePollBlocks> visited;
  while (!pending.empty()) {
    auto* block = pending.pop_back_val();
    if (!loop.contains(block) || !visited.insert(block).second) {
      continue;
    }
    // 只累计无请求路径可执行的块；冷恢复、物化和错误处理不消耗免轮询工作预算。
    region.instructions += block->size();
    region.blocks.push_back(block);
    if (std::ranges::any_of(*block, [](const auto& instruction) {
          const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
          return call != nullptr && !llvm::isa<llvm::IntrinsicInst>(call);
        })) {
      return {};
    }
    auto* branch = llvm::dyn_cast<llvm::CondBrInst>(block->getTerminator());
    if (branch == nullptr) {
      pending.append(llvm::succ_begin(block), llvm::succ_end(block));
      continue;
    }
    const auto first_failed = failures.contains(branch->getSuccessor(0));
    const auto second_failed = failures.contains(branch->getSuccessor(1));
    if (branch->getMetadata(kNativeBoundedPollMetadata) != nullptr) {
      auto* ready = llvm::dyn_cast_or_null<llvm::ConstantInt>(
          PollReadyValue(branch->getCondition(), block->getModule()->getDataLayout()));
      if (ready == nullptr) {
        if (first_failed == second_failed) {
          return {};
        }
        // LLVM 可把请求检查与缓存守卫合并；只有真实冷恢复边才能进入稳定性证明，
        // 不能把守卫的未知条件直接当作无请求常量删除。
        const auto ready_edge = static_cast<unsigned>(first_failed);
        region.guards.push_back({.branch = branch, .ready_edge = ready_edge, .guarded_poll = true});
        pending.push_back(branch->getSuccessor(ready_edge));
        continue;
      }
      const auto ready_edge = static_cast<unsigned>(ready->isZero());
      region.polls.push_back({.branch = branch, .ready_edge = ready_edge});
      // 标记的慢边负责请求处理；纯叶调用的正常计算不包含那里的运行时调用。
      pending.push_back(branch->getSuccessor(ready_edge));
    } else if (first_failed || second_failed) {
      if (first_failed == second_failed) {
        return {};
      }
      const auto ready_edge = static_cast<unsigned>(first_failed);
      region.guards.push_back({.branch = branch, .ready_edge = ready_edge});
      pending.push_back(branch->getSuccessor(ready_edge));
    } else {
      pending.append(llvm::succ_begin(block), llvm::succ_end(block));
    }
  }
  return region;
}

using GuardEqualities = llvm::DenseMap<llvm::Value*, llvm::Constant*>;

bool IsPollConditionInstruction(const llvm::Instruction& instruction) {
  return llvm::isa<llvm::LoadInst, llvm::GetElementPtrInst, llvm::CastInst, llvm::BinaryOperator,
                   llvm::ICmpInst>(instruction);
}

void CollectGuardEqualities(llvm::Value* value, bool ready, GuardEqualities& equalities) {
  llvm::Value* inverted = nullptr;
  if (llvm::PatternMatch::match(value,
                                llvm::PatternMatch::m_Not(llvm::PatternMatch::m_Value(inverted)))) {
    CollectGuardEqualities(inverted, !ready, equalities);
    return;
  }
  if (auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(value)) {
    if ((ready && binary->getOpcode() == llvm::Instruction::And) ||
        (!ready && binary->getOpcode() == llvm::Instruction::Or)) {
      CollectGuardEqualities(binary->getOperand(0), ready, equalities);
      CollectGuardEqualities(binary->getOperand(1), ready, equalities);
    }
    return;
  }
  const auto* comparison = llvm::dyn_cast<llvm::ICmpInst>(value);
  if (comparison == nullptr || (ready ? comparison->getPredicate() != llvm::CmpInst::ICMP_EQ
                                      : comparison->getPredicate() != llvm::CmpInst::ICMP_NE)) {
    return;
  }
  auto* left = comparison->getOperand(0);
  auto* right = comparison->getOperand(1);
  if (llvm::isa<llvm::Constant>(left)) {
    std::swap(left, right);
  }
  if (auto* constant = llvm::dyn_cast<llvm::Constant>(right)) {
    equalities.try_emplace(left, constant);
  }
}

enum class GuardLoadMode : std::uint8_t { Stable, BoundedPoll };

bool IsDeferredPollLoad(const llvm::LoadInst& load) {
  return !load.isVolatile() && load.isAtomic() &&
         load.getOrdering() == llvm::AtomicOrdering::Monotonic &&
         load.getMetadata(kNativePollReadyStateMetadata) != nullptr;
}

bool IsGuardLoadPreserved(const llvm::LoadInst& load,
                          const llvm::MemoryLocation& location,
                          const llvm::StoreInst& store,
                          llvm::Constant* expected,
                          llvm::AAResults& aliases) {
  const auto overlap = aliases.alias(location, llvm::MemoryLocation::get(&store));
  if (overlap == llvm::AliasResult::NoAlias) {
    return true;
  }
  // 已检查的标签可在片段内重复写入同一值。宽存储只有 MustAlias 才能
  // 保留此证明，防止相同常量的错位部分覆盖改变读取结果。
  return expected != nullptr && store.isSimple() && store.getValueOperand() == expected &&
         (load.getType()->isIntegerTy(kValueKindBits) || overlap == llvm::AliasResult::MustAlias);
}

bool CanCopyGuardLoad(llvm::LoadInst& load,
                      const BoundedRegion& region,
                      const GuardEqualities& equalities,
                      llvm::AAResults& aliases,
                      GuardLoadMode mode) {
  const auto deferred_request = mode == GuardLoadMode::BoundedPoll && IsDeferredPollLoad(load);
  if (deferred_request) {
    // 标记的请求字只能由控制协议写入；它不要求片段内值恒定，而是与独立轮询
    // 一样允许延迟到下个有界片段。其余缓存读取仍须通过完整的别名证明。
    return true;
  }
  if (!load.isSimple()) {
    return false;
  }
  if (load.getMetadata(llvm::LLVMContext::MD_invariant_load) != nullptr) {
    return true;
  }
  const auto location = llvm::MemoryLocation::get(&load);
  auto* expected = equalities.lookup(&load);
  for (auto* block : region.blocks) {
    for (auto& instruction : *block) {
      if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        if (!IsGuardLoadPreserved(load, location, *store, expected, aliases)) {
          return false;
        }
      } else if (auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        if (llvm::isModSet(aliases.getModRefInfo(call, location))) {
          return false;
        }
      } else if (const auto* observed = llvm::dyn_cast<llvm::LoadInst>(&instruction);
                 observed != nullptr && IsDeferredPollLoad(*observed)) {
        // LLVM 将原子读取也视为潜在写入；这里仅忽略已由有界轮询协议推迟的
        // 请求字观察。它不会写入脚本堆，实际请求处理不在成功片段内。
        continue;
      } else if (instruction.mayWriteToMemory()) {
        return false;
      }
    }
  }
  return true;
}

template <class LoadPredicate>
bool CanCopyCondition(llvm::Value* value,
                      const llvm::Loop& loop,
                      llvm::SmallPtrSetImpl<llvm::Value*>& copied,
                      const LoadPredicate& can_copy_load) {
  if (copied.contains(value)) {
    return true;
  }
  auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
  if (instruction == nullptr || !loop.contains(instruction)) {
    return true;
  }
  if (auto* load = llvm::dyn_cast<llvm::LoadInst>(instruction)) {
    if (!can_copy_load(*load)) {
      return false;
    }
  } else if (!IsPollConditionInstruction(*instruction)) {
    return false;
  }
  if (!std::ranges::all_of(instruction->operands(), [&](const auto& operand) {
        return CanCopyCondition(operand.get(), loop, copied, can_copy_load);
      })) {
    return false;
  }
  copied.insert(value);
  return true;
}

bool HasSuccessfulDominance(llvm::BasicBlock* before,
                            llvm::BasicBlock* after,
                            const llvm::Loop& loop,
                            const llvm::SmallPtrSetImpl<llvm::BasicBlock*>& blocks,
                            const llvm::DominatorTree& dominators) {
  if (dominators.dominates(before, after)) {
    return true;
  }
  llvm::SmallPtrSet<llvm::BasicBlock*, kInlinePollBlocks> visited;
  llvm::SmallVector<llvm::BasicBlock*, kInlinePollBlocks> pending{after};
  while (!pending.empty()) {
    auto* block = pending.pop_back_val();
    if (block == before || !blocks.contains(block) || !visited.insert(block).second) {
      continue;
    }
    if (block == loop.getHeader()) {
      return false;
    }
    pending.append(llvm::pred_begin(block), llvm::pred_end(block));
  }
  return true;
}

bool CanMoveGuards(llvm::Loop& loop,
                   BoundedRegion& region,
                   llvm::AAResults& aliases,
                   llvm::DominatorTree& dominators,
                   llvm::SmallPtrSetImpl<llvm::Value*>& copied) {
  auto& guards = region.guards;
  if (guards.empty()) {
    return true;
  }
  // 冷查找可在后续参数检查前重新汇入；完整 CFG 的支配关系会包含这条已排除的边。
  // 按成功片段的支配关系排序，互不支配的条件分支仍不能合并为入口守卫链。
  const llvm::SmallPtrSet<llvm::BasicBlock*, kInlinePollBlocks> blocks(region.blocks.begin(),
                                                                       region.blocks.end());
  const auto dominates = [&](const auto& left, const auto& right) {
    return HasSuccessfulDominance(left.branch->getParent(), right.branch->getParent(), loop, blocks,
                                  dominators);
  };
  for (std::size_t index = 1; index < guards.size(); ++index) {
    for (auto position = index; position != 0; --position) {
      auto& previous = guards[position - 1];
      auto& current = guards[position];
      if (dominates(previous, current)) {
        break;
      }
      if (!dominates(current, previous)) {
        return false;
      }
      std::swap(previous, current);
    }
  }
  GuardEqualities equalities;
  for (const auto& guard : guards) {
    CollectGuardEqualities(guard.branch->getCondition(), guard.ready_edge == 0, equalities);
  }
  if (!std::ranges::all_of(guards, [&](const auto& guard) {
        const auto mode = guard.guarded_poll ? GuardLoadMode::BoundedPoll : GuardLoadMode::Stable;
        return CanCopyCondition(guard.branch->getCondition(), loop, copied, [&](auto& load) {
          return CanCopyGuardLoad(load, region, equalities, aliases, mode);
        });
      })) {
    return false;
  }
  // 冷 binder 可改变数组长度，成功片段内则没有这类写入。只在同一
  // 别名证明成立时，允许片段入口重新读取原 header 的计数上界。
  const GuardEqualities no_equalities;
  for (auto& instruction : *loop.getHeader()) {
    if (auto* comparison = llvm::dyn_cast<llvm::ICmpInst>(&instruction)) {
      for (auto* operand : comparison->operand_values()) {
        CanCopyCondition(operand, loop, copied, [&](auto& load) {
          return CanCopyGuardLoad(load, region, no_equalities, aliases, GuardLoadMode::Stable);
        });
      }
    }
  }
  return true;
}

llvm::Value* CopyPollCondition(llvm::Value* value,
                               const llvm::Loop& loop,
                               llvm::Instruction* before,
                               llvm::ValueToValueMapTy& copied) {
  auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
  if (instruction == nullptr || !loop.contains(instruction)) {
    return value;
  }
  if (const auto found = copied.find(value); found != copied.end()) {
    return found->second;
  }
  auto* clone = instruction->clone();
  for (unsigned index = 0; index < instruction->getNumOperands(); ++index) {
    clone->setOperand(index,
                      CopyPollCondition(instruction->getOperand(index), loop, before, copied));
  }
  clone->insertBefore(before->getIterator());
  copied.insert({value, clone});
  return clone;
}

void ErasePollCondition(llvm::Value* value) {
  auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
  if (instruction == nullptr || !instruction->use_empty() ||
      !IsPollConditionInstruction(*instruction)) {
    return;
  }
  llvm::SmallVector<llvm::WeakTrackingVH> operands(instruction->operand_values());
  instruction->eraseFromParent();
  for (const auto& operand : operands) {
    if (operand != nullptr) {
      ErasePollCondition(operand);
    }
  }
}

std::uint64_t PollBudget(std::uint64_t instructions) {
  return std::min(kMaximumUnpolledIterations, kMaximumUnpolledInstructions / instructions);
}

llvm::Value* BoundedDistance(llvm::IRBuilder<>& builder,
                             const CountedLoop& counted,
                             llvm::Value* start,
                             llvm::Value* bound,
                             std::uint64_t limit) {
  auto* distance =
      counted.direction == 1 ? builder.CreateSub(bound, start) : builder.CreateSub(start, bound);
  return builder.CreateICmpULE(distance, builder.getInt64(limit));
}

llvm::Value* BoundedTripCount(llvm::Loop& loop,
                              llvm::ScalarEvolution& evolution,
                              const std::optional<CountedLoop>& counted,
                              std::uint64_t limit) {
  auto* preheader = loop.getLoopPreheader();
  if (preheader == nullptr) {
    return nullptr;
  }
  llvm::IRBuilder<> guard(preheader->getTerminator());
  const auto maximum = evolution.getSmallConstantMaxTripCount(&loop);
  if (maximum != 0 && maximum <= limit) {
    return guard.getTrue();
  }
  if (!counted) {
    return nullptr;
  }
  auto* start = counted->index->getIncomingValueForBlock(preheader);
  return BoundedDistance(guard, *counted, start, counted->bound, limit);
}

void RemoveBoundedPolls(std::span<const BoundedPoll> polls, const llvm::ValueToValueMapTy& mapped) {
  for (const auto& poll : polls) {
    // 原循环保留安全点，但不再作为下一轮优化的复制候选。
    poll.branch->setMetadata(kNativeBoundedPollMetadata, nullptr);
    auto* cloned = llvm::cast<llvm::CondBrInst>(mapped.lookup(poll.branch));
    auto* condition = cloned->getCondition();
    auto* ready = cloned->getSuccessor(poll.ready_edge);
    // 克隆循环也向共享恢复出口的 PHI 增加了输入；删除轮询边时同步退休该输入。
    cloned->getSuccessor(1 - poll.ready_edge)->removePredecessor(cloned->getParent());
    llvm::UncondBrInst::Create(ready, cloned->getIterator());
    cloned->eraseFromParent();
    ErasePollCondition(condition);
  }
}

llvm::Value* WithoutPendingPolls(llvm::Loop& loop,
                                 llvm::Value* bounded,
                                 std::span<const BoundedPoll> polls,
                                 llvm::Instruction* before,
                                 llvm::ValueToValueMapTy& conditions) {
  llvm::IRBuilder<> guard(before);
  for (const auto& poll : polls) {
    auto* condition = CopyPollCondition(poll.branch->getCondition(), loop, before, conditions);
    auto* ready = poll.ready_edge == 0 ? condition : guard.CreateNot(condition);
    bounded = guard.CreateAnd(bounded, ready);
  }
  return bounded;
}

void VersionBoundedLoop(llvm::Loop& loop,
                        llvm::Value* bounded,
                        std::span<const BoundedPoll> polls) {
  auto* preheader = loop.getLoopPreheader();
  auto* terminator = preheader->getTerminator();
  llvm::ValueToValueMapTy conditions;
  bounded = WithoutPendingPolls(loop, bounded, polls, terminator, conditions);
  llvm::ValueToValueMapTy mapped;
  VersionLoop(loop, *bounded, mapped, ".bounded");
  RemoveBoundedPolls(polls, mapped);
}

bool CanStripMine(const CountedLoop& counted, const BoundedRegion& region) {
  const auto& loop = *counted.loop;
  const auto* header = loop.getHeader();
  // 分块边界会重新执行循环条件；条件前不得存在可观察的写入或调用。
  if (counted.condition->getParent() != header ||
      std::ranges::any_of(*header, [](const auto& instruction) {
        return !instruction.isTerminator() && instruction.mayHaveSideEffects() &&
               instruction.getMetadata(kNativeRootClearMetadata) == nullptr;
      })) {
    return false;
  }
  const auto& guards = region.guards;
  if (guards.empty()) {
    return true;
  }
  llvm::DenseMap<llvm::BasicBlock*, llvm::BasicBlock*> ready_successors;
  for (const auto& poll : region.polls) {
    ready_successors.insert({poll.branch->getParent(), poll.branch->getSuccessor(poll.ready_edge)});
  }
  for (const auto& guard : guards) {
    ready_successors.insert(
        {guard.branch->getParent(), guard.branch->getSuccessor(guard.ready_edge)});
  }
  // 守卫已按成功路径的支配关系排序。每次非空迭代必须到达最后一级守卫，
  // 不能越过可跳过它的分支、break 或 return 提前解引用本轮不使用的参数。
  const auto* last_guard = guards.back().branch->getParent();
  llvm::SmallVector<llvm::BasicBlock*, kInlinePollBlocks> pending{
      counted.condition->getSuccessor(1 - counted.exit_edge),
  };
  llvm::SmallPtrSet<llvm::BasicBlock*, kInlinePollBlocks> visited;
  while (!pending.empty()) {
    auto* block = pending.pop_back_val();
    if (block == last_guard) {
      continue;
    }
    if (block == header || !loop.contains(block)) {
      return false;
    }
    if (!visited.insert(block).second) {
      continue;
    }
    if (auto* ready = ready_successors.lookup(block)) {
      pending.push_back(ready);
    } else {
      pending.append(llvm::succ_begin(block), llvm::succ_end(block));
    }
  }
  return true;
}

struct BoundedLoopValues final {
  llvm::ValueToValueMapTy instructions;
  llvm::ValueToValueMapTy conditions;
};

void GuardBoundedLoop(llvm::Loop& loop,
                      std::span<const BoundedPoll> guards,
                      BoundedLoopValues& values,
                      llvm::CondBrInst& entry) {
  if (guards.empty()) {
    return;
  }
  auto& mapped = values.instructions;
  auto& conditions = values.conditions;
  auto* generic = loop.getHeader();
  auto* fast = entry.getSuccessor(0);
  auto* previous = entry.getParent();
  llvm::BasicBlock* first = nullptr;
  for (const auto& guard : guards) {
    auto* block = llvm::BasicBlock::Create(generic->getContext(), "poll.chunk.guard",
                                           generic->getParent(), fast);
    if (first == nullptr) {
      first = block;
    } else {
      previous->getTerminator()->setSuccessor(0, block);
    }
    auto* branch = llvm::CondBrInst::Create(llvm::ConstantInt::getTrue(generic->getContext()), fast,
                                            generic, block);
    auto* condition = CopyPollCondition(guard.branch->getCondition(), loop, branch, conditions);
    if (guard.ready_edge != 0) {
      condition = llvm::IRBuilder<>(branch).CreateNot(condition);
    }
    branch->setCondition(condition);
    for (auto& phi : generic->phis()) {
      phi.addIncoming(phi.getIncomingValueForBlock(entry.getParent()), block);
    }
    previous = block;
  }
  entry.setSuccessor(0, first);
  for (auto& phi : fast->phis()) {
    const auto incoming = static_cast<unsigned>(phi.getBasicBlockIndex(entry.getParent()));
    phi.setIncomingBlock(incoming, previous);
  }
  // 每级守卫保留原短路顺序，只有长度匹配后才读取数组元素。成功片段复用
  // 已证明稳定的地址和标签；任一不匹配从本轮原状态进入完整 binder 路径。
  for (const auto& [original, moved] : conditions) {
    const auto* instruction = llvm::dyn_cast<llvm::Instruction>(original);
    if (instruction == nullptr || llvm::isa<llvm::PHINode>(instruction) ||
        !loop.contains(instruction)) {
      continue;
    }
    if (auto* cloned = llvm::dyn_cast_or_null<llvm::Instruction>(mapped.lookup(original))) {
      cloned->replaceAllUsesWith(moved);
      if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(cloned);
          load != nullptr && load->isAtomic()) {
        // 已移到片段入口的请求观察不能只替换其结果；原子 load 不会被普通 DCE 删除。
        cloned->eraseFromParent();
      }
    }
  }
  RemoveBoundedPolls(guards, mapped);
}

void StripMineBoundedLoop(const CountedLoop& counted,
                          const BoundedRegion& region,
                          std::uint64_t limit) {
  auto& loop = *counted.loop;
  auto* header = loop.getHeader();
  auto* preheader = loop.getLoopPreheader();
  auto* latch = loop.getLoopLatch();
  auto* function = header->getParent();
  auto& context = function->getContext();
  auto* outer = llvm::BasicBlock::Create(context, "poll.chunk", function, header);
  auto* exit = llvm::BasicBlock::Create(context, "poll.chunk.exit", function);
  BoundedLoopValues values;
  auto& mapped = values.instructions;
  auto& conditions = values.conditions;
  CloneLoopBody(loop, mapped, ".bounded");
  auto* fast = llvm::cast<llvm::BasicBlock>(mapped.lookup(header));
  llvm::IRBuilder<> guard(outer);
  llvm::Value* start = nullptr;
  for (auto& phi : header->phis()) {
    auto* state = guard.CreatePHI(phi.getType(), 3, phi.getName() + ".chunk");
    state->addIncoming(phi.getIncomingValueForBlock(preheader), preheader);
    state->addIncoming(phi.getIncomingValueForBlock(latch), latch);
    auto* cloned = llvm::cast<llvm::PHINode>(mapped.lookup(&phi));
    state->addIncoming(cloned, exit);
    conditions.insert({&phi, state});
    const auto initial = static_cast<unsigned>(phi.getBasicBlockIndex(preheader));
    phi.setIncomingBlock(initial, outer);
    phi.setIncomingValue(initial, state);
    phi.removeIncomingValue(latch, false);
    const auto cloned_initial = static_cast<unsigned>(cloned->getBasicBlockIndex(preheader));
    cloned->setIncomingBlock(cloned_initial, outer);
    cloned->setIncomingValue(cloned_initial, state);
    if (&phi == counted.index) {
      start = state;
    }
  }
  preheader->getTerminator()->replaceSuccessorWith(header, outer);
  latch->getTerminator()->replaceSuccessorWith(header, outer);
  auto* placeholder = guard.CreateBr(header);
  auto* full_bound = CopyPollCondition(counted.bound, loop, placeholder, conditions);
  guard.SetInsertPoint(placeholder);
  auto* clipped = counted.direction == 1 ? guard.CreateAdd(start, guard.getInt64(limit))
                                         : guard.CreateSub(start, guard.getInt64(limit));
  auto* bound = guard.CreateSelect(BoundedDistance(guard, counted, start, full_bound, limit),
                                   full_bound, clipped);
  auto* comparison = llvm::cast<llvm::ICmpInst>(mapped.lookup(counted.comparison));
  const auto bound_operand = counted.comparison->getOperand(0) == counted.index ? 1U : 0U;
  comparison->setOperand(bound_operand, bound);
  auto* condition = llvm::cast<llvm::CondBrInst>(mapped.lookup(counted.condition));
  auto* normal_exit = condition->getSuccessor(counted.exit_edge);
  normal_exit->removePredecessor(condition->getParent());
  condition->setSuccessor(counted.exit_edge, exit);
  llvm::IRBuilder<>(exit).CreateBr(outer);
  auto* more = counted.direction == 1 ? guard.CreateICmpSLT(start, full_bound)
                                      : guard.CreateICmpSGT(start, full_bound);
  auto* branch = guard.CreateCondBr(more, fast, header);
  placeholder->eraseFromParent();
  branch->setCondition(WithoutPendingPolls(loop, more, region.polls, branch, conditions));
  RemoveBoundedPolls(region.polls, mapped);
  GuardBoundedLoop(loop, region.guards, values, *branch);
}

class BoundedPollPass : public llvm::PassInfoMixin<BoundedPollPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    auto& evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
    auto& aliases = analyses.getResult<llvm::AAManager>(function);
    struct Plan final {
      llvm::Loop* loop;
      llvm::Value* bounded;
      BoundedRegion region;
      std::optional<CountedLoop> counted;
      std::uint64_t limit;
    };
    std::vector<Plan> plans;
    for (auto* loop : loops.getLoopsInPreorder()) {
      auto region = FindBoundedRegion(*loop);
      auto& polls = region.polls;
      llvm::SmallPtrSet<llvm::Value*, kInlineGuardValues> copied;
      if ((polls.empty() && std::ranges::none_of(region.guards, &BoundedPoll::guarded_poll)) ||
          !std::ranges::all_of(polls, [&](const auto& poll) {
            return CanCopyCondition(poll.branch->getCondition(), *loop, copied,
                                    [](const auto&) { return true; });
          })) {
        continue;
      }
      llvm::SmallPtrSet<llvm::Value*, kInlineGuardValues> stable;
      if (!CanMoveGuards(*loop, region, aliases, dominators, stable)) {
        continue;
      }
      const auto counted = MatchCountedLoop(*loop, dominators, &stable);
      const auto limit = PollBudget(region.instructions);
      if (counted && limit != 0 && CanStripMine(*counted, region)) {
        plans.push_back({
            .loop = loop,
            .bounded = nullptr,
            .region = std::move(region),
            .counted = counted,
            .limit = limit,
        });
        continue;
      }
      if (!region.guards.empty()) {
        continue;
      }
      auto* bounded = BoundedTripCount(*loop, evolution, counted, limit);
      if (bounded != nullptr) {
        plans.push_back({
            .loop = loop,
            .bounded = bounded,
            .region = std::move(region),
            .counted = std::nullopt,
            .limit = limit,
        });
      }
    }
    for (const auto& plan : plans) {
      if (plan.counted) {
        StripMineBoundedLoop(*plan.counted, plan.region, plan.limit);
      } else {
        VersionBoundedLoop(*plan.loop, plan.bounded, plan.region.polls);
      }
    }
    if (!plans.empty()) {
      llvm::removeUnreachableBlocks(function);
      return llvm::PreservedAnalyses::none();
    }
    return llvm::PreservedAnalyses::all();
  }
};

struct Reduction {
  llvm::PHINode* accumulator;
  llvm::Value* term;
  CheckedUpdate update;
  bool check_prefix;
};

void AddUpdateInstructions(llvm::SmallPtrSetImpl<llvm::Value*>& allowed,
                           const CheckedUpdate& update) {
  allowed.insert(update.call);
  allowed.insert(update.value);
  allowed.insert(update.overflow);
  allowed.insert(update.branch->getCondition());
}

void AddStepInstructions(AllowedInstructions& allowed, const CountedLoop& counted) {
  if (counted.step) {
    AddUpdateInstructions(allowed, *counted.step);
  } else {
    allowed.insert(counted.increment);
  }
}

llvm::PHINode* FindAccumulator(const CountedLoop& counted) {
  const auto& loop = *counted.loop;
  llvm::PHINode* accumulator = nullptr;
  for (auto& phi : loop.getHeader()->phis()) {
    if (&phi == counted.index) {
      continue;
    }
    if (accumulator != nullptr || !phi.getType()->isIntegerTy(kIntegerBits)) {
      return nullptr;
    }
    accumulator = &phi;
  }
  return accumulator;
}

bool IsMonotoneTerm(const CountedLoop& counted,
                    llvm::Value* term,
                    llvm::ScalarEvolution& evolution) {
  const auto& loop = *counted.loop;
  if (term == counted.index) {
    auto* start = counted.index->getIncomingValueForBlock(loop.getLoopPreheader());
    const auto* start_range = evolution.getSCEV(start);
    const auto* bound_range = evolution.getSCEV(counted.bound);
    // 与 Go loopbce、Dart RangeAnalysis 一样，先用循环条件约束 induction range。
    // 跨零的等差项可能让中间前缀先溢出再回到范围内，不能只检查最终值。
    return counted.direction == 1 ? evolution.isKnownNonNegative(start_range) ||
                                        evolution.isKnownNonPositive(bound_range)
                                  : evolution.isKnownNonPositive(start_range) ||
                                        evolution.isKnownNonNegative(bound_range);
  }
  return loop.isLoopInvariant(term);
}

bool IsReductionInstruction(const llvm::Instruction& instruction,
                            const CountedLoop& counted,
                            const Reduction& reduction,
                            const AllowedInstructions& allowed) {
  if (const auto* branch = llvm::dyn_cast<llvm::CondBrInst>(&instruction)) {
    if (branch != counted.condition && (!counted.step || branch != counted.step->branch) &&
        branch != reduction.update.branch) {
      return false;
    }
  } else if (!llvm::isa<llvm::UncondBrInst>(instruction) && !allowed.contains(&instruction)) {
    return false;
  }
  auto* normal_exit = counted.condition->getSuccessor(counted.exit_edge);
  for (const auto* user : instruction.users()) {
    const auto* use = llvm::cast<llvm::Instruction>(user);
    if (!counted.loop->contains(use) &&
        (use->getParent() != normal_exit ||
         (&instruction != reduction.accumulator && &instruction != counted.index))) {
      return false;
    }
  }
  return true;
}

void AddCountedUpdateInstructions(AllowedInstructions& allowed,
                                  const CountedLoop& counted,
                                  const CheckedUpdate& update) {
  allowed.insert(counted.index);
  allowed.insert(counted.comparison);
  allowed.insert(counted.condition->getCondition());
  AddStepInstructions(allowed, counted);
  AddUpdateInstructions(allowed, update);
}

bool IsReductionBody(const CountedLoop& counted, const Reduction& reduction) {
  const auto& loop = *counted.loop;
  auto* accumulator = reduction.accumulator;
  const auto& update = reduction.update;
  AllowedInstructions allowed;
  allowed.insert(accumulator);
  AddCountedUpdateInstructions(allowed, counted, update);
  for (auto* block : loop.blocks()) {
    for (auto& instruction : *block) {
      if (!IsReductionInstruction(instruction, counted, reduction, allowed)) {
        return false;
      }
    }
  }
  // 累加器只能参与自己的 recurrence；比较、再次使用等都会观察中间值。
  return std::ranges::all_of(accumulator->users(), [&](const auto* user) {
    return !loop.contains(llvm::cast<llvm::Instruction>(user)) || user == update.call;
  });
}

std::optional<Reduction> MatchReduction(const CountedLoop& counted,
                                        llvm::ScalarEvolution& evolution,
                                        llvm::DominatorTree& dominators) {
  const auto& loop = *counted.loop;
  if (!loop.getSubLoops().empty()) {
    return std::nullopt;
  }
  auto* accumulator = FindAccumulator(counted);
  if (accumulator == nullptr) {
    return std::nullopt;
  }
  const auto update = MatchUpdate(accumulator->getIncomingValueForBlock(loop.getLoopLatch()), loop);
  if (!update || !dominators.dominates(counted.condition->getParent(), update->call->getParent())) {
    return std::nullopt;
  }
  if (std::ranges::any_of(update->failed->phis(), [&](const auto& phi) {
        return !loop.isLoopInvariant(phi.getIncomingValueForBlock(update->branch->getParent()));
      })) {
    return std::nullopt;
  }
  auto* left = update->call->getArgOperand(0);
  auto* term = update->call->getArgOperand(1);
  if (term == accumulator &&
      update->call->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow) {
    std::swap(left, term);
  }
  if (left != accumulator || (term != counted.index && !loop.isLoopInvariant(term))) {
    return std::nullopt;
  }
  const Reduction reduction{
      .accumulator = accumulator,
      .term = term,
      .update = *update,
      .check_prefix = !IsMonotoneTerm(counted, term, evolution),
  };
  return IsReductionBody(counted, reduction) ? std::optional(reduction) : std::nullopt;
}

void ReplaceUpdate(const CheckedUpdate& update, llvm::Value* result, llvm::Value* overflow) {
  update.value->replaceAllUsesWith(result);
  update.overflow->replaceAllUsesWith(overflow);
  update.value->eraseFromParent();
  update.overflow->eraseFromParent();
  update.call->eraseFromParent();
}

void ReplaceUpdate(const CheckedUpdate& update, llvm::Value* result) {
  ReplaceUpdate(update, result, llvm::ConstantInt::getFalse(update.call->getContext()));
}

bool HasBoundedReductionPrefixes(const CountedLoop& counted,
                                 llvm::PHINode& accumulator,
                                 const CheckedUpdate& update,
                                 llvm::ScalarEvolution& evolution) {
  auto* left = update.call->getArgOperand(0);
  auto* term = update.call->getArgOperand(1);
  const auto subtract = update.call->getIntrinsicID() == llvm::Intrinsic::ssub_with_overflow;
  if (term == &accumulator && !subtract) {
    std::swap(left, term);
  }
  if (left != &accumulator) {
    return false;
  }
  const auto range = [&](llvm::Value* value) {
    return evolution.getSignedRange(evolution.getSCEV(value));
  };
  const auto start =
      range(counted.index->getIncomingValueForBlock(counted.loop->getLoopPreheader()));
  const auto bound = range(counted.bound);
  // 使用 i128 计算整个循环的最大迭代数和所有前缀的上下界，不能只证明片段内
  // 的增量，也不能只检查最终和；正负项抵消时仍须保留首次溢出的可观察位置。
  constexpr unsigned kProofBits = kIntegerBits * 2;
  auto count = counted.direction == 1
                   ? bound.getSignedMax().sext(kProofBits) - start.getSignedMin().sext(kProofBits)
                   : start.getSignedMax().sext(kProofBits) - bound.getSignedMin().sext(kProofBits);
  if (count.isNegative()) {
    count = llvm::APInt(kProofBits, 0);
  }
  const auto initial =
      range(accumulator.getIncomingValueForBlock(counted.loop->getLoopPreheader()));
  const auto terms = range(term);
  const auto minimum =
      subtract ? -terms.getSignedMax().sext(kProofBits) : terms.getSignedMin().sext(kProofBits);
  const auto maximum =
      subtract ? -terms.getSignedMin().sext(kProofBits) : terms.getSignedMax().sext(kProofBits);
  const auto lower = initial.getSignedMin().sext(kProofBits) +
                     (minimum.isNegative() ? minimum * count : llvm::APInt(kProofBits, 0));
  const auto upper = initial.getSignedMax().sext(kProofBits) +
                     (maximum.isStrictlyPositive() ? maximum * count : llvm::APInt(kProofBits, 0));
  return lower.sge(llvm::APInt::getSignedMinValue(kIntegerBits).sext(kProofBits)) &&
         upper.sle(llvm::APInt::getSignedMaxValue(kIntegerBits).sext(kProofBits));
}

class BoundedReductionPass : public llvm::PassInfoMixin<BoundedReductionPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    auto& evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
    llvm::SmallVector<CheckedUpdate> updates;
    for (auto* loop : loops.getLoopsInPreorder()) {
      const auto counted = MatchCountedLoop(*loop, dominators);
      if (!counted) {
        continue;
      }
      for (auto& phi : loop->getHeader()->phis()) {
        if (&phi == counted->index || !phi.getType()->isIntegerTy(kIntegerBits)) {
          continue;
        }
        const auto update = MatchUpdate(phi.getIncomingValueForBlock(loop->getLoopLatch()), *loop);
        const llvm::BasicBlockEdge continuing(
            counted->condition->getParent(),
            counted->condition->getSuccessor(1 - counted->exit_edge));
        if (update && dominators.dominates(continuing, update->call->getParent()) &&
            HasBoundedReductionPrefixes(*counted, phi, *update, evolution)) {
          updates.push_back(*update);
        }
      }
    }
    for (const auto& update : updates) {
      llvm::IRBuilder<> builder(update.call);
      auto* left = update.call->getArgOperand(0);
      auto* right = update.call->getArgOperand(1);
      auto* value = update.call->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow
                        ? builder.CreateNSWAdd(left, right)
                        : builder.CreateNSWSub(left, right);
      ReplaceUpdate(update, value);
    }
    return updates.empty() ? llvm::PreservedAnalyses::all() : llvm::PreservedAnalyses::none();
  }
};

llvm::Value* ReductionPrefixFits(llvm::IRBuilder<>& builder,
                                 const CountedLoop& counted,
                                 llvm::Value* initial,
                                 llvm::Intrinsic::ID operation) {
  auto* wide = builder.getInt128Ty();
  auto* zero = llvm::ConstantInt::get(wide, 0);
  auto* start = builder.CreateSExt(
      counted.index->getIncomingValueForBlock(counted.loop->getLoopPreheader()), wide);
  auto* bound = builder.CreateSExt(counted.bound, wide);
  // 等差项跨零时，最小/最大前缀位于符号切换处。只查最终和会漏掉先溢出、
  // 后抵消的情况。将这个极值与最终和一起检查，不依赖分块索引的 SCEV 符号推断。
  auto* end = builder.CreateBinaryIntrinsic(
      counted.direction == 1 ? llvm::Intrinsic::smin : llvm::Intrinsic::smax, bound, zero);
  auto* distance =
      counted.direction == 1 ? builder.CreateNSWSub(end, start) : builder.CreateNSWSub(start, end);
  auto* count = builder.CreateBinaryIntrinsic(llvm::Intrinsic::smax, distance, zero);
  auto* last = counted.direction == 1 ? builder.CreateNSWSub(end, llvm::ConstantInt::get(wide, 1))
                                      : builder.CreateNSWAdd(end, llvm::ConstantInt::get(wide, 1));
  auto* paired = builder.CreateNSWAdd(start, last);
  auto* sum = builder.CreateAShr(builder.CreateNSWMul(count, paired), 1);
  auto* extreme = operation == llvm::Intrinsic::sadd_with_overflow
                      ? builder.CreateNSWAdd(initial, sum)
                      : builder.CreateNSWSub(initial, sum);
  auto* narrowed = builder.CreateTrunc(extreme, builder.getInt64Ty());
  return builder.CreateICmpEQ(extreme, builder.CreateSExt(narrowed, wide));
}

void WidenReduction(const CountedLoop& counted, const Reduction& reduction) {
  auto& loop = *counted.loop;
  auto* accumulator = reduction.accumulator;
  auto* header = loop.getHeader();
  auto* preheader = loop.getLoopPreheader();
  llvm::IRBuilder<> entry(preheader->getTerminator());
  auto* wide_type = entry.getInt128Ty();
  auto* initial = entry.CreateSExt(accumulator->getIncomingValueForBlock(preheader), wide_type);
  const auto operation = reduction.update.call->getIntrinsicID();
  auto* prefix_fits = reduction.check_prefix
                          ? ReductionPrefixFits(entry, counted, initial, operation)
                          : entry.getTrue();
  auto* wide = llvm::PHINode::Create(wide_type, 2, "sum.wide", accumulator->getIterator());
  wide->addIncoming(initial, preheader);
  llvm::IRBuilder<> body(reduction.update.call);
  auto* term = body.CreateSExt(reduction.term, wide_type);
  // 严格比较、单位步长最多执行 2^64-1 次。任意 int64 初值和 int64
  // 加/减项的所有前缀都能以有符号 i128 精确表示，NSW 不依赖最终 i64 检查。
  auto* next = operation == llvm::Intrinsic::sadd_with_overflow
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
  auto* fits =
      exit.CreateAnd(prefix_fits, exit.CreateICmpEQ(wide, exit.CreateSExt(narrow, wide_type)));
  // LCSSA 可能为外层逻辑帧标志等不变量保留出口 PHI。新增范围失败边使用相同输入；
  // 依赖当前迭代值的失败输入已在 MatchReduction 排除，不能延后其可观察前缀。
  for (auto& phi : reduction.update.failed->phis()) {
    phi.addIncoming(phi.getIncomingValueForBlock(reduction.update.branch->getParent()), check);
  }
  // 重用原失败块，保留错误类型、源位置、保根与 catch/finally 路径。
  // MatchReduction 已排除所有能在失败路径观察中间迭代状态的 SSA uses。
  exit.CreateCondBr(fits, normal_exit, reduction.update.failed);
}

struct FieldReduction {
  llvm::LoadInst* load;
  llvm::StoreInst* store;
  CheckedUpdate update;
};

struct FieldAccesses {
  llvm::LoadInst* load = nullptr;
  llvm::StoreInst* store = nullptr;
  llvm::StoreInst* tag = nullptr;
};

bool CanIncludeFieldAccess(FieldAccesses& accesses, llvm::Instruction& instruction) {
  if (auto* read = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    if (accesses.load != nullptr || !read->isSimple() ||
        !read->getType()->isIntegerTy(kIntegerBits)) {
      return false;
    }
    accesses.load = read;
  }
  auto* write = llvm::dyn_cast<llvm::StoreInst>(&instruction);
  if (write == nullptr) {
    return true;
  }
  if (!write->isSimple()) {
    return false;
  }
  if (write->getMetadata(kNativeRootClearMetadata) != nullptr) {
    return true;
  }
  if (write->getValueOperand()->getType()->isIntegerTy(kIntegerBits) && accesses.store == nullptr) {
    accesses.store = write;
    return true;
  }
  const auto* kind = llvm::dyn_cast<llvm::ConstantInt>(write->getValueOperand());
  if (kind == nullptr || !kind->getType()->isIntegerTy(kValueKindBits) ||
      kind->getZExtValue() != static_cast<std::uint8_t>(runtime::ValueKind::Integer) ||
      accesses.tag != nullptr) {
    return false;
  }
  accesses.tag = write;
  return true;
}

std::optional<FieldAccesses> MatchFieldAccesses(const llvm::Loop& loop) {
  FieldAccesses accesses;
  for (auto* block : loop.blocks()) {
    for (auto& instruction : *block) {
      if (!CanIncludeFieldAccess(accesses, instruction)) {
        return std::nullopt;
      }
    }
  }
  if (accesses.load == nullptr || accesses.store == nullptr ||
      accesses.load->getPointerOperand() != accesses.store->getPointerOperand()) {
    return std::nullopt;
  }
  return accesses;
}

bool IsFieldInstruction(const llvm::Instruction& instruction,
                        const CountedLoop& counted,
                        const CheckedUpdate& update,
                        const AllowedInstructions& allowed,
                        const llvm::Value* base) {
  if (instruction.getMetadata(kNativeRootClearMetadata) != nullptr) {
    return true;
  }
  if (llvm::isa<llvm::UncondBrInst>(instruction)) {
    return true;
  }
  if (const auto* branch = llvm::dyn_cast<llvm::CondBrInst>(&instruction)) {
    if (branch == counted.condition || (counted.step && branch == counted.step->branch) ||
        branch == update.branch || llvm::isa<llvm::ConstantInt>(branch->getCondition())) {
      return true;
    }
    const auto* check = llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
    // 保留第一次迭代的空引用检查；零次迭代不访问对象。
    return check != nullptr && check->isEquality() && check->getOperand(0) == base &&
           llvm::isa<llvm::ConstantPointerNull>(check->getOperand(1));
  }
  const auto is_address = llvm::isa<llvm::GetElementPtrInst>(instruction);
  if (!allowed.contains(&instruction) && !is_address && !llvm::isa<llvm::ICmpInst>(instruction)) {
    return false;
  }
  // 循环索引或临时值若在 catch 等路径可见，不能跳过其中间状态。
  return is_address || std::ranges::all_of(instruction.users(), [&](const auto* user) {
           return counted.loop->contains(llvm::cast<llvm::Instruction>(user));
         });
}

bool IsFieldReductionBody(const CountedLoop& counted,
                          const FieldAccesses& accesses,
                          const CheckedUpdate& update,
                          const llvm::Value* base) {
  AllowedInstructions allowed;
  allowed.insert(accesses.load);
  allowed.insert(accesses.store);
  if (accesses.tag != nullptr) {
    allowed.insert(accesses.tag);
  }
  AddCountedUpdateInstructions(allowed, counted, update);
  for (auto* block : counted.loop->blocks()) {
    for (auto& instruction : *block) {
      if (!IsFieldInstruction(instruction, counted, update, allowed, base)) {
        return false;
      }
    }
  }
  return true;
}

std::optional<FieldReduction> MatchFieldReduction(const CountedLoop& counted,
                                                  llvm::DominatorTree& dominators,
                                                  llvm::AAResults& aliases) {
  const auto& loop = *counted.loop;
  if (!loop.getSubLoops().empty()) {
    return std::nullopt;
  }
  const auto accesses = MatchFieldAccesses(loop);
  if (!accesses) {
    return std::nullopt;
  }
  auto* load = accesses->load;
  auto* store = accesses->store;
  const auto update = MatchUpdate(store->getValueOperand(), loop);
  if (!update || !update->failed->phis().empty() ||
      !dominators.dominates(store, counted.increment) ||
      !dominators.dominates(counted.condition->getParent(), load->getParent())) {
    return std::nullopt;
  }
  auto* term = llvm::dyn_cast<llvm::ConstantInt>(update->call->getArgOperand(1));
  if (update->call->getArgOperand(0) != load || term == nullptr || !term->isOne()) {
    return std::nullopt;
  }
  const auto& layout = load->getModule()->getDataLayout();
  std::int64_t value_offset = 0;
  auto* base =
      llvm::GetPointerBaseWithConstantOffset(load->getPointerOperand(), value_offset, layout);
  // 标签可以位于独立区域；只要求常量标签写入不会覆盖归约载荷。
  // 私有借用字段使用类型化槽，没有标签写入。
  if (accesses->tag != nullptr && !aliases.isNoAlias(llvm::MemoryLocation::get(load),
                                                     llvm::MemoryLocation::get(accesses->tag))) {
    return std::nullopt;
  }
  if (!loop.isLoopInvariant(base) || !IsFieldReductionBody(counted, *accesses, *update, base)) {
    return std::nullopt;
  }
  return FieldReduction{.load = load, .store = store, .update = *update};
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
  ReplaceUpdate(field.update, result, overflow);
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
  if (counted.step) {
    ReplaceUpdate(*counted.step, counted.bound);
  } else {
    counted.increment->replaceAllUsesWith(counted.bound);
    counted.increment->eraseFromParent();
  }
}

class FieldLoopPass : public llvm::PassInfoMixin<FieldLoopPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    bool changed = false;
    for (auto* loop : loops.getLoopsInPreorder()) {
      const auto counted = MatchCountedLoop(*loop, dominators);
      if (!counted) {
        continue;
      }
      auto& aliases = analyses.getResult<llvm::AAManager>(function);
      const auto field = MatchFieldReduction(*counted, dominators, aliases);
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
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
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
        const auto reduction = MatchReduction(*counted, evolution, dominators);
        if (counted->step || reduction) {
          plans.push_back({.counted = *counted, .reduction = reduction});
        }
      }
    }
    for (const auto& plan : plans) {
      const auto& counted = plan.counted;
      if (plan.reduction) {
        WidenReduction(counted, *plan.reduction);
      }
      if (!counted.step) {
        continue;
      }
      const auto& step = *counted.step;
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

void EmitBoundedLeafGuard(const GeneratedFunction& function) {
  auto& body = *function.leaf;
  auto& guard = *function.leaf_guard;
  llvm::DominatorTree dominators(body);
  PromoteLocalAllocations(body, dominators);
  llvm::LoopInfo loops(dominators);
  llvm::AssumptionCache assumptions(body);
  for (auto* loop : loops) {
    llvm::simplifyLoop(loop, &dominators, &loops, nullptr, &assumptions, nullptr, false);
  }
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(guard.getContext(), "entry", &guard));
  const auto nested = loops.getLoopsInPreorder();
  const auto instructions = static_cast<std::uint64_t>(body.getInstructionCount());
  if (nested.empty()) {
    builder.CreateRet(builder.getInt1(instructions <= kMaximumUnpolledInstructions));
    return;
  }
  if (nested.size() != 1) {
    builder.CreateRet(builder.getFalse());
    return;
  }
  const auto counted = MatchCountedLoop(*nested.front(), dominators);
  if (!counted) {
    builder.CreateRet(builder.getFalse());
    return;
  }
  const auto input = [&](llvm::Value* value) -> llvm::Value* {
    if (llvm::isa<llvm::ConstantInt>(value)) {
      return value;
    }
    const auto* argument = llvm::dyn_cast<llvm::Argument>(value);
    return argument == nullptr ? nullptr : guard.getArg(argument->getArgNo());
  };
  auto* start = input(counted->index->getIncomingValueForBlock(counted->loop->getLoopPreheader()));
  auto* bound = input(counted->bound);
  if (start == nullptr || bound == nullptr) {
    builder.CreateRet(builder.getFalse());
    return;
  }
  const auto limit = PollBudget(instructions);
  builder.CreateRet(BoundedDistance(builder, *counted, start, bound, limit));
}

void AddBoundedPollPasses(llvm::FunctionPassManager& passes) {
  passes.addPass(llvm::PromotePass());
  passes.addPass(llvm::LoopSimplifyPass());
  // 根槽清理不会修改数组头；用别名分析合并隔着这类写入的长度读取，
  // 在循环旋转前去掉重复越界检查，保留后续分块和参数守卫的规范计数形状。
  passes.addPass(llvm::EarlyCSEPass(true));
  passes.addPass(llvm::DCEPass());
  passes.addPass(llvm::SimplifyCFGPass());
  passes.addPass(llvm::LoopSimplifyPass());
  passes.addPass(llvm::LCSSAPass());
  // 先完成规范计数循环的字段折叠和精确归约；后续 CVP/JumpThreading 可把零边界改为
  // 符号测试并剥离首轮，届时原有的有界前缀和证明已不再可见。
  passes.addPass(FieldLoopPass());
  passes.addPass(IntegerLoopPass());
  // 内联的标量计算先消费 load 范围，再在分块产生循环携带初值之前证明完整归约。
  passes.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
  passes.addPass(llvm::InstCombinePass());
  passes.addPass(llvm::CorrelatedValuePropagationPass());
  passes.addPass(llvm::JumpThreadingPass());
  passes.addPass(llvm::InstCombinePass());
  passes.addPass(llvm::SimplifyCFGPass());
  passes.addPass(llvm::LoopSimplifyPass());
  passes.addPass(BoundedReductionPass());
  passes.addPass(llvm::SimplifyCFGPass());
  passes.addPass(llvm::LoopSimplifyPass());
  passes.addPass(llvm::LCSSAPass());
  passes.addPass(BoundedPollPass());
  passes.addPass(llvm::SimplifyCFGPass());
  passes.addPass(llvm::LoopSimplifyPass());
  passes.addPass(llvm::EarlyCSEPass());
  passes.addPass(llvm::DCEPass());
}

void AddIntegerLoopPasses(llvm::FunctionPassManager& passes) {
  AddBoundedPollPasses(passes);
  // 分块复制和 CFG 化简会让循环值直接流入外层 PHI；归约检查与改写依赖
  // 专属出口上的 LCSSA，必须在这些变换后重新建立该契约。
  passes.addPass(llvm::LCSSAPass());
  passes.addPass(FieldLoopPass());
  passes.addPass(IntegerLoopPass());
}

}  // namespace vela::compiler::native
