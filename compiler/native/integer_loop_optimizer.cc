#include "compiler/native/integer_loop_optimizer.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
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
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/LCSSA.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "compiler/native/function_emitter.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {
namespace {

constexpr auto kIntegerBits = std::numeric_limits<std::uint64_t>::digits;
constexpr auto kValueKindBits = std::numeric_limits<std::uint8_t>::digits;
// 常见的归约只有两组受查更新，内联容纳其指令集合。
constexpr unsigned kInlineAllowedValues = 16;
using AllowedInstructions = llvm::SmallPtrSet<llvm::Value*, kInlineAllowedValues>;

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
  llvm::BranchInst* condition;
  unsigned exit_edge;
  llvm::Instruction* increment;
  std::optional<CheckedUpdate> step;
  int direction;
};

std::optional<CountedLoop> MatchLoopExit(llvm::Loop& loop,
                                         llvm::BasicBlock* block,
                                         llvm::DominatorTree& dominators) {
  auto* branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
  if (branch == nullptr || !branch->isConditional()) {
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
      !index->getType()->isIntegerTy(kIntegerBits) || !loop.isLoopInvariant(bound)) {
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
  auto* left = increment->getOperand(0);
  auto* right = increment->getOperand(1);
  const bool adding = step ? step->call->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow
                           : plain->getOpcode() == llvm::Instruction::Add;
  if (right == index && adding) {
    std::swap(left, right);
  }
  auto* constant = llvm::dyn_cast<llvm::ConstantInt>(right);
  if (left != index || constant == nullptr || (!constant->isOne() && !constant->isMinusOne())) {
    return std::nullopt;
  }
  const int direction = (adding ? 1 : -1) * (constant->isOne() ? 1 : -1);
  if ((direction == 1 && predicate == llvm::CmpInst::ICMP_SLT) ||
      (direction == -1 && predicate == llvm::CmpInst::ICMP_SGT)) {
    return CountedLoop{
        .loop = &loop,
        .index = index,
        .bound = bound,
        .comparison = comparison,
        .condition = branch,
        .exit_edge = exit_edge,
        .increment = increment,
        .step = step,
        .direction = direction,
    };
  }
  return std::nullopt;
}

std::optional<CountedLoop> MatchCountedLoop(llvm::Loop& loop, llvm::DominatorTree& dominators) {
  if (!loop.isLoopSimplifyForm()) {
    return std::nullopt;
  }
  llvm::SmallVector<llvm::BasicBlock*> exits;
  loop.getExitingBlocks(exits);
  for (auto* block : exits) {
    const auto counted = MatchLoopExit(loop, block, dominators);
    if (counted) {
      return counted;
    }
  }
  return std::nullopt;
}

// 纯计算内核最多延迟一个有界片段的停止请求。入口保留请求检查；包含宿主、分配、
// 其他脚本调用或调试逐行观测的循环仍执行原安全点。限制同时覆盖迭代数与代码体积。
constexpr std::uint64_t kMaximumUnpolledIterations = 1024;
constexpr std::uint64_t kMaximumUnpolledInstructions = 131072;

std::vector<llvm::BranchInst*> BoundedPolls(const llvm::Loop& loop) {
  std::vector<llvm::BranchInst*> polls;
  if (!loop.getSubLoops().empty()) {
    return polls;
  }
  for (auto* block : loop.blocks()) {
    for (auto& instruction : *block) {
      if (auto* branch = llvm::dyn_cast<llvm::BranchInst>(&instruction);
          branch != nullptr && branch->getMetadata(kNativeBoundedPollMetadata) != nullptr) {
        polls.push_back(branch);
      }
      const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call == nullptr || llvm::isa<llvm::IntrinsicInst>(call)) {
        continue;
      }
      const auto* callee = call->getCalledFunction();
      if (callee == nullptr || callee->getName() != "vela_native_v3_safepoint") {
        return {};
      }
    }
  }
  return polls;
}

bool CanCopyPollCondition(llvm::Value* value, const llvm::Loop& loop) {
  const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
  if (instruction == nullptr || !loop.contains(instruction)) {
    return true;
  }
  if (!llvm::isa<llvm::LoadInst, llvm::GetElementPtrInst, llvm::CastInst, llvm::BinaryOperator,
                 llvm::ICmpInst>(instruction)) {
    return false;
  }
  return std::ranges::all_of(instruction->operands(), [&](const auto& operand) {
    return CanCopyPollCondition(operand.get(), loop);
  });
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
      !llvm::isa<llvm::LoadInst, llvm::GetElementPtrInst, llvm::CastInst, llvm::BinaryOperator,
                 llvm::ICmpInst>(instruction)) {
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

std::uint64_t PollBudget(const llvm::Loop& loop) {
  std::uint64_t instructions = 0;
  for (const auto* block : loop.blocks()) {
    instructions += block->size();
  }
  return std::min(kMaximumUnpolledIterations, kMaximumUnpolledInstructions / instructions);
}

llvm::Value* BoundedTripCount(llvm::Loop& loop,
                              llvm::ScalarEvolution& evolution,
                              llvm::DominatorTree& dominators) {
  auto* preheader = loop.getLoopPreheader();
  if (preheader == nullptr) {
    return nullptr;
  }
  const auto limit = PollBudget(loop);
  llvm::IRBuilder<> guard(preheader->getTerminator());
  const auto maximum = evolution.getSmallConstantMaxTripCount(&loop);
  if (maximum != 0 && maximum <= limit) {
    return guard.getTrue();
  }
  const auto counted = MatchCountedLoop(loop, dominators);
  if (!counted) {
    return nullptr;
  }
  auto* start = counted->index->getIncomingValueForBlock(preheader);
  auto* distance = counted->direction == 1 ? guard.CreateSub(counted->bound, start)
                                           : guard.CreateSub(start, counted->bound);
  return guard.CreateICmpULE(distance, guard.getInt64(limit));
}

void CopyLoop(llvm::Loop& loop, llvm::ValueToValueMapTy& mapped) {
  auto* function = loop.getHeader()->getParent();
  llvm::SmallVector<llvm::BasicBlock*> clones;
  for (auto* block : loop.blocks()) {
    auto* cloned = llvm::CloneBasicBlock(block, mapped, ".bounded", function);
    mapped.insert({block, cloned});
    clones.push_back(cloned);
  }
  llvm::remapInstructionsInBlocks(clones, mapped);
  for (auto* block : loop.blocks()) {
    llvm::SmallPtrSet<llvm::BasicBlock*, 4> exits;
    for (auto* successor : llvm::successors(block)) {
      if (loop.contains(successor) || !exits.insert(successor).second) {
        continue;
      }
      for (auto& phi : successor->phis()) {
        auto* incoming = phi.getIncomingValueForBlock(block);
        const auto found = mapped.find(incoming);
        // 循环外的 SSA 值仍由原定义支配克隆；MapValue 对未映射的局部值返回空。
        auto* cloned_incoming =
            found == mapped.end() ? incoming : static_cast<llvm::Value*>(found->second);
        phi.addIncoming(cloned_incoming, llvm::cast<llvm::BasicBlock>(mapped.lookup(block)));
      }
    }
  }
}

void RemoveBoundedPolls(std::span<llvm::BranchInst* const> polls,
                        const llvm::ValueToValueMapTy& mapped) {
  for (auto* poll : polls) {
    // 原循环保留安全点，但不再作为下一轮优化的复制候选。
    poll->setMetadata(kNativeBoundedPollMetadata, nullptr);
    auto* cloned = llvm::cast<llvm::BranchInst>(mapped.lookup(poll));
    auto* condition = cloned->getCondition();
    auto* ready = cloned->getSuccessor(1);
    // 克隆循环也向共享恢复出口的 PHI 增加了输入；删除轮询边时同步退休该输入。
    cloned->getSuccessor(0)->removePredecessor(cloned->getParent());
    llvm::BranchInst::Create(ready, cloned->getIterator());
    cloned->eraseFromParent();
    ErasePollCondition(condition);
  }
}

llvm::Value* WithoutPendingPolls(llvm::Loop& loop,
                                 llvm::Value* bounded,
                                 std::span<llvm::BranchInst* const> polls,
                                 llvm::Instruction* before) {
  llvm::IRBuilder<> guard(before);
  llvm::ValueToValueMapTy conditions;
  for (auto* poll : polls) {
    auto* pending = CopyPollCondition(poll->getCondition(), loop, before, conditions);
    bounded = guard.CreateAnd(bounded, guard.CreateNot(pending));
  }
  return bounded;
}

void VersionBoundedLoop(llvm::Loop& loop,
                        llvm::Value* bounded,
                        std::span<llvm::BranchInst* const> polls) {
  auto* preheader = loop.getLoopPreheader();
  auto* terminator = preheader->getTerminator();
  bounded = WithoutPendingPolls(loop, bounded, polls, terminator);
  llvm::ValueToValueMapTy mapped;
  CopyLoop(loop, mapped);
  RemoveBoundedPolls(polls, mapped);
  auto* fast = llvm::cast<llvm::BasicBlock>(mapped.lookup(loop.getHeader()));
  llvm::BranchInst::Create(fast, loop.getHeader(), bounded, terminator->getIterator());
  terminator->eraseFromParent();
}

bool CanStripMine(const CountedLoop& counted) {
  const auto* header = counted.loop->getHeader();
  // 分块边界会重新执行循环条件；条件前不得存在可观察的写入或调用。
  return PollBudget(*counted.loop) != 0 && counted.condition->getParent() == header &&
         std::ranges::none_of(*header, [](const auto& instruction) {
           return !instruction.isTerminator() && instruction.mayHaveSideEffects();
         });
}

void StripMineBoundedLoop(const CountedLoop& counted, std::span<llvm::BranchInst* const> polls) {
  auto& loop = *counted.loop;
  auto* header = loop.getHeader();
  auto* preheader = loop.getLoopPreheader();
  auto* latch = loop.getLoopLatch();
  auto* function = header->getParent();
  auto& context = function->getContext();
  auto* outer = llvm::BasicBlock::Create(context, "poll.chunk", function, header);
  auto* exit = llvm::BasicBlock::Create(context, "poll.chunk.exit", function);
  llvm::ValueToValueMapTy mapped;
  CopyLoop(loop, mapped);
  auto* fast = llvm::cast<llvm::BasicBlock>(mapped.lookup(header));
  llvm::IRBuilder<> guard(outer);
  llvm::Value* start = nullptr;
  for (auto& phi : header->phis()) {
    auto* state = guard.CreatePHI(phi.getType(), 3, phi.getName() + ".chunk");
    state->addIncoming(phi.getIncomingValueForBlock(preheader), preheader);
    state->addIncoming(phi.getIncomingValueForBlock(latch), latch);
    auto* cloned = llvm::cast<llvm::PHINode>(mapped.lookup(&phi));
    state->addIncoming(cloned, exit);
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
  const auto limit = PollBudget(loop);
  auto* distance = counted.direction == 1 ? guard.CreateSub(counted.bound, start)
                                          : guard.CreateSub(start, counted.bound);
  auto* clipped = counted.direction == 1 ? guard.CreateAdd(start, guard.getInt64(limit))
                                         : guard.CreateSub(start, guard.getInt64(limit));
  auto* bound = guard.CreateSelect(guard.CreateICmpULE(distance, guard.getInt64(limit)),
                                   counted.bound, clipped);
  auto* comparison = llvm::cast<llvm::ICmpInst>(mapped.lookup(counted.comparison));
  const auto bound_operand = counted.comparison->getOperand(0) == counted.index ? 1U : 0U;
  comparison->setOperand(bound_operand, bound);
  auto* condition = llvm::cast<llvm::BranchInst>(mapped.lookup(counted.condition));
  auto* normal_exit = condition->getSuccessor(counted.exit_edge);
  normal_exit->removePredecessor(condition->getParent());
  condition->setSuccessor(counted.exit_edge, exit);
  llvm::IRBuilder<>(exit).CreateBr(outer);
  auto* more = counted.direction == 1 ? guard.CreateICmpSLT(start, counted.bound)
                                      : guard.CreateICmpSGT(start, counted.bound);
  auto* branch = guard.CreateCondBr(more, fast, header);
  branch->setCondition(WithoutPendingPolls(loop, more, polls, branch));
  RemoveBoundedPolls(polls, mapped);
}

class BoundedPollPass : public llvm::PassInfoMixin<BoundedPollPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    auto& evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
    struct Plan final {
      llvm::Loop* loop;
      llvm::Value* bounded;
      std::vector<llvm::BranchInst*> polls;
      std::optional<CountedLoop> counted;
    };
    std::vector<Plan> plans;
    for (auto* loop : loops.getLoopsInPreorder()) {
      const auto polls = BoundedPolls(*loop);
      if (polls.empty() || !std::ranges::all_of(polls, [&](const auto* poll) {
            return CanCopyPollCondition(poll->getCondition(), *loop);
          })) {
        continue;
      }
      const auto counted = MatchCountedLoop(*loop, dominators);
      if (counted && CanStripMine(*counted)) {
        plans.push_back({.loop = loop, .bounded = nullptr, .polls = polls, .counted = counted});
        continue;
      }
      auto* bounded = BoundedTripCount(*loop, evolution, dominators);
      if (bounded != nullptr) {
        plans.push_back(
            {.loop = loop, .bounded = bounded, .polls = polls, .counted = std::nullopt});
      }
    }
    for (const auto& plan : plans) {
      if (plan.counted) {
        StripMineBoundedLoop(*plan.counted, plan.polls);
      } else {
        VersionBoundedLoop(*plan.loop, plan.bounded, plan.polls);
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
  if (const auto* branch = llvm::dyn_cast<llvm::BranchInst>(&instruction)) {
    if (branch->isConditional() && branch != counted.condition &&
        (!counted.step || branch != counted.step->branch) && branch != reduction.update.branch) {
      return false;
    }
  } else if (!allowed.contains(&instruction)) {
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

bool IsReductionBody(const CountedLoop& counted, const Reduction& reduction) {
  const auto& loop = *counted.loop;
  auto* accumulator = reduction.accumulator;
  const auto& update = reduction.update;
  AllowedInstructions allowed;
  allowed.insert(counted.index);
  allowed.insert(accumulator);
  allowed.insert(counted.comparison);
  allowed.insert(counted.condition->getCondition());
  AddStepInstructions(allowed, counted);
  AddUpdateInstructions(allowed, update);
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
  const Reduction reduction{.accumulator = accumulator,
                            .term = term,
                            .update = *update,
                            .check_prefix = !IsMonotoneTerm(counted, term, evolution)};
  return IsReductionBody(counted, reduction) ? std::optional(reduction) : std::nullopt;
}

void ReplaceUpdate(const CheckedUpdate& update, llvm::Value* result) {
  update.value->replaceAllUsesWith(result);
  update.overflow->replaceAllUsesWith(llvm::ConstantInt::getFalse(update.call->getContext()));
  update.value->eraseFromParent();
  update.overflow->eraseFromParent();
  update.call->eraseFromParent();
}

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
  if (const auto* branch = llvm::dyn_cast<llvm::BranchInst>(&instruction)) {
    if (!branch->isConditional() || branch == counted.condition ||
        (counted.step && branch == counted.step->branch) || branch == update.branch ||
        llvm::isa<llvm::ConstantInt>(branch->getCondition())) {
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
  allowed.insert(counted.index);
  allowed.insert(counted.comparison);
  allowed.insert(counted.condition->getCondition());
  AddStepInstructions(allowed, counted);
  AddUpdateInstructions(allowed, update);
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
    auto& aliases = analyses.getResult<llvm::AAManager>(function);
    bool changed = false;
    for (auto* loop : loops.getLoopsInPreorder()) {
      const auto counted = MatchCountedLoop(*loop, dominators);
      if (!counted) {
        continue;
      }
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
  llvm::SmallVector<llvm::AllocaInst*> locals;
  for (auto& instruction : body.getEntryBlock()) {
    auto* local = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
    if (local != nullptr && llvm::isAllocaPromotable(local)) {
      locals.push_back(local);
    }
  }
  llvm::PromoteMemToReg(locals, dominators);
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
  auto* distance =
      counted->direction == 1 ? builder.CreateSub(bound, start) : builder.CreateSub(start, bound);
  const auto limit =
      std::min(kMaximumUnpolledIterations, kMaximumUnpolledInstructions / instructions);
  builder.CreateRet(builder.CreateICmpULE(distance, builder.getInt64(limit)));
}

void AddIntegerLoopPasses(llvm::FunctionPassManager& passes) {
  passes.addPass(llvm::PromotePass());
  passes.addPass(llvm::LoopSimplifyPass());
  passes.addPass(llvm::EarlyCSEPass());
  passes.addPass(llvm::DCEPass());
  passes.addPass(llvm::SimplifyCFGPass());
  passes.addPass(llvm::LoopSimplifyPass());
  passes.addPass(llvm::LCSSAPass());
  passes.addPass(BoundedPollPass());
  passes.addPass(llvm::SimplifyCFGPass());
  passes.addPass(llvm::LoopSimplifyPass());
  passes.addPass(llvm::EarlyCSEPass());
  passes.addPass(llvm::DCEPass());
  // 分块复制和 CFG 化简会让循环值直接流入外层 PHI；归约检查与改写依赖
  // 专属出口上的 LCSSA，必须在这些变换后重新建立该契约。
  passes.addPass(llvm::LCSSAPass());
  passes.addPass(FieldLoopPass());
  passes.addPass(IntegerLoopPass());
}

}  // namespace vela::compiler::native
