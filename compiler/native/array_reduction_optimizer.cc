#include "compiler/native/array_reduction_optimizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/LCSSA.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/LoopUtils.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>

#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/memory_regions.h"
#include "runtime/native/native_memory_layout.h"

namespace vela::compiler::native {
namespace {

constexpr std::uint64_t kSmallIntegerMask = std::numeric_limits<std::int32_t>::max();
constexpr std::uint64_t kTrialInstructions = 16;
constexpr std::uint32_t kTrialInterleaveCount = 4;

struct ArrayReduction final {
  llvm::Loop* loop = nullptr;
  llvm::PHINode* index = nullptr;
  llvm::PHINode* total = nullptr;
  llvm::BinaryOperator* increment = nullptr;
  llvm::IntrinsicInst* update = nullptr;
  llvm::ExtractValueInst* sum = nullptr;
  llvm::ExtractValueInst* overflow = nullptr;
  llvm::GetElementPtrInst* address = nullptr;
  llvm::LoadInst* element = nullptr;
  llvm::Value* bound = nullptr;
  llvm::BasicBlock* normal_exit = nullptr;
  llvm::SmallVector<llvm::StoreInst*, 4> clears;
  std::uint64_t limit = 0;
};

std::optional<ArrayReduction> MatchReductionPhis(ArrayReduction reduction) {
  auto& loop = *reduction.loop;
  std::size_t count = 0;
  for (auto& phi : loop.getHeader()->phis()) {
    ++count;
    if (!phi.getType()->isIntegerTy(64) || phi.getNumIncomingValues() != 2) {
      return std::nullopt;
    }
    auto* incoming = phi.getIncomingValueForBlock(loop.getLoopLatch());
    if (auto* increment = llvm::dyn_cast<llvm::BinaryOperator>(incoming);
        increment != nullptr && increment->getOpcode() == llvm::Instruction::Add &&
        increment->hasNoSignedWrap() && increment->getOperand(0) == &phi) {
      const auto* step = llvm::dyn_cast<llvm::ConstantInt>(increment->getOperand(1));
      const auto* start = llvm::dyn_cast<llvm::ConstantInt>(
          phi.getIncomingValueForBlock(loop.getLoopPreheader()));
      if (step == nullptr || !step->isOne() || start == nullptr ||
          start->getValue().ugt(std::numeric_limits<std::uint32_t>::max())) {
        return std::nullopt;
      }
      reduction.index = &phi;
      reduction.increment = increment;
      continue;
    }
    auto* sum = llvm::dyn_cast<llvm::ExtractValueInst>(incoming);
    if (sum == nullptr || sum->getNumIndices() != 1 || sum->getIndices().front() != 0) {
      return std::nullopt;
    }
    auto* update = llvm::dyn_cast<llvm::IntrinsicInst>(sum->getAggregateOperand());
    if (update == nullptr || update->getIntrinsicID() != llvm::Intrinsic::sadd_with_overflow) {
      return std::nullopt;
    }
    reduction.total = &phi;
    reduction.sum = sum;
    reduction.update = update;
  }
  if (count != 2 || reduction.index == nullptr || reduction.total == nullptr) {
    return std::nullopt;
  }
  return reduction;
}

std::optional<ArrayReduction> MatchReductionExits(ArrayReduction reduction) {
  auto& loop = *reduction.loop;
  auto* latch = loop.getLoopLatch();
  auto* branch = llvm::dyn_cast<llvm::CondBrInst>(latch->getTerminator());
  auto* comparison = branch == nullptr ? nullptr : llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
  if (comparison == nullptr || !comparison->isEquality()) {
    return std::nullopt;
  }
  const auto continuing = branch->getSuccessor(0) == loop.getHeader() ? 0U : 1U;
  if (branch->getSuccessor(continuing) != loop.getHeader() ||
      loop.contains(branch->getSuccessor(1 - continuing)) ||
      (comparison->getPredicate() == llvm::CmpInst::ICMP_NE) != (continuing == 0)) {
    return std::nullopt;
  }
  auto* bound = comparison->getOperand(1);
  if (comparison->getOperand(0) != reduction.increment) {
    if (bound != reduction.increment) {
      return std::nullopt;
    }
    bound = comparison->getOperand(0);
  }
  if (!loop.isLoopInvariant(bound)) {
    return std::nullopt;
  }
  reduction.bound = bound;
  reduction.normal_exit = branch->getSuccessor(1 - continuing);
  for (auto* user : reduction.update->users()) {
    if (user == reduction.sum) {
      continue;
    }
    auto* extract = llvm::dyn_cast<llvm::ExtractValueInst>(user);
    if (extract == nullptr || extract->getNumIndices() != 1 ||
        extract->getIndices().front() != 1 || reduction.overflow != nullptr) {
      return std::nullopt;
    }
    reduction.overflow = extract;
  }
  auto* checked = llvm::dyn_cast<llvm::CondBrInst>(loop.getHeader()->getTerminator());
  if (reduction.overflow == nullptr || checked == nullptr ||
      checked->getCondition() != reduction.overflow || loop.contains(checked->getSuccessor(0)) ||
      checked->getSuccessor(1) != latch) {
    return std::nullopt;
  }
  return reduction;
}

std::optional<ArrayReduction> MatchNativeArrayStorage(ArrayReduction reduction) {
  auto& loop = *reduction.loop;
  auto* left = reduction.update->getArgOperand(0);
  auto* term = reduction.update->getArgOperand(1);
  if (term == reduction.total) {
    std::swap(left, term);
  }
  auto* element = llvm::dyn_cast<llvm::LoadInst>(term);
  if (left != reduction.total || element == nullptr || !element->isSimple() ||
      element->getParent() != loop.getHeader() ||
      element->getMetadata(llvm::LLVMContext::MD_tbaa) !=
          NativeMemoryTag(element->getContext(), MemoryRegion::Elements)) {
    return std::nullopt;
  }
  auto* address = llvm::dyn_cast<llvm::GetElementPtrInst>(element->getPointerOperand());
  auto* bound = llvm::dyn_cast<llvm::ZExtInst>(reduction.bound);
  if (address == nullptr || address->getNumIndices() != 1 ||
      address->getOperand(1) != reduction.index || bound == nullptr ||
      !bound->getSrcTy()->isIntegerTy(32)) {
    return std::nullopt;
  }
  const auto& layout = element->getModule()->getDataLayout();
  const auto stride = layout.getTypeAllocSize(address->getSourceElementType());
  auto* data = llvm::dyn_cast<llvm::LoadInst>(address->getPointerOperand());
  auto* length = llvm::dyn_cast<llvm::LoadInst>(bound->getOperand(0));
  if (stride.isScalable() || stride.getFixedValue() != sizeof(std::int64_t) || data == nullptr ||
      length == nullptr || !data->isSimple() || !length->isSimple() ||
      !loop.isLoopInvariant(data) || !loop.isLoopInvariant(length)) {
    return std::nullopt;
  }
  std::int64_t data_offset = 0;
  std::int64_t length_offset = 0;
  auto* data_owner = llvm::GetPointerBaseWithConstantOffset(data->getPointerOperand(), data_offset,
                                                          layout);
  auto* length_owner = llvm::GetPointerBaseWithConstantOffset(length->getPointerOperand(),
                                                            length_offset, layout);
  using runtime::NativeMemoryLayout;
  // Elements 的未标记 int64 读取及同一数组的 data/uint32 length 共同证明完整读取范围；
  // 不能仅凭当前单次读取的越界检查推断其余投机读取也有效。
  if (data_owner != length_owner || data_offset != NativeMemoryLayout::kArrayData ||
      length_offset != NativeMemoryLayout::kObjectLength) {
    return std::nullopt;
  }
  reduction.element = element;
  reduction.address = address;
  return reduction;
}

std::optional<ArrayReduction> MatchPureReductionBody(ArrayReduction reduction) {
  auto& loop = *reduction.loop;
  llvm::SmallPtrSet<const llvm::Value*, 16> allowed{
      reduction.index, reduction.total, reduction.increment, reduction.update, reduction.sum,
      reduction.overflow, reduction.address, reduction.element,
      loop.getHeader()->getTerminator(), loop.getLoopLatch()->getTerminator(),
      llvm::cast<llvm::CondBrInst>(loop.getLoopLatch()->getTerminator())->getCondition(),
  };
  std::uint64_t instructions = kTrialInstructions;
  for (auto* block : loop.blocks()) {
    instructions += block->size();
    for (auto& instruction : *block) {
      if (allowed.contains(&instruction)) {
        continue;
      }
      auto* clear = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      const auto* zero = clear == nullptr ? nullptr : llvm::dyn_cast<llvm::Constant>(clear->getValueOperand());
      if (clear == nullptr || !clear->isSimple() || zero == nullptr || !zero->isNullValue() ||
          clear->getMetadata(kNativeRootClearMetadata) == nullptr ||
          !loop.isLoopInvariant(clear->getPointerOperand()) ||
          clear->getMetadata(llvm::LLVMContext::MD_tbaa) !=
              NativeMemoryTag(clear->getContext(), MemoryRegion::Roots)) {
        return std::nullopt;
      }
      reduction.clears.push_back(clear);
    }
  }
  const auto mapped = [&](const llvm::Value* value) {
    return loop.isLoopInvariant(value) || value == reduction.index || value == reduction.total ||
           value == reduction.increment || value == reduction.sum || value == reduction.element;
  };
  if (!std::ranges::all_of(reduction.normal_exit->phis(), [&](const auto& phi) {
        return mapped(phi.getIncomingValueForBlock(loop.getLoopLatch()));
      })) {
    return std::nullopt;
  }
  reduction.limit = std::min(kNativeMaximumUnpolledIterations / 2,
                              kNativeMaximumUnpolledInstructions / (2 * instructions));
  if (reduction.limit == 0) {
    return std::nullopt;
  }
  return reduction;
}

std::optional<ArrayReduction> MatchReduction(llvm::Loop& loop) {
  if (loop.getBlocks().size() != 2 || !loop.getSubLoops().empty() ||
      loop.getLoopPreheader() == nullptr || loop.getLoopLatch() == nullptr) {
    return std::nullopt;
  }
  auto reduction = MatchReductionPhis({.loop = &loop, .clears = {}});
  if (!reduction) return std::nullopt;
  reduction = MatchReductionExits(std::move(*reduction));
  if (!reduction) return std::nullopt;
  reduction = MatchNativeArrayStorage(std::move(*reduction));
  if (!reduction) return std::nullopt;
  return MatchPureReductionBody(std::move(*reduction));
}

void EmitTrial(const ArrayReduction& reduction) {
  auto& loop = *reduction.loop;
  auto* header = loop.getHeader();
  auto* preheader = loop.getLoopPreheader();
  auto* old_entry = preheader->getTerminator();
  auto* function = header->getParent();
  auto& context = function->getContext();
  auto* trial = llvm::BasicBlock::Create(context, "array.sum.trial", function, header);
  auto* validate = llvm::BasicBlock::Create(context, "array.sum.validate", function, header);
  auto* commit = llvm::BasicBlock::Create(context, "array.sum.commit", function, header);
  auto* start = reduction.index->getIncomingValueForBlock(preheader);
  auto* initial = reduction.total->getIncomingValueForBlock(preheader);
  llvm::IRBuilder<> guard(old_entry);
  auto* distance = guard.CreateSub(reduction.bound, start);
  auto* nonempty = guard.CreateICmpNE(distance, guard.getInt64(0));
  auto* bounded = guard.CreateICmpULE(distance, guard.getInt64(reduction.limit));
  guard.CreateCondBr(guard.CreateAnd(nonempty, bounded), trial, header);
  old_entry->eraseFromParent();

  llvm::IRBuilder<> body(trial);
  auto* index = body.CreatePHI(body.getInt64Ty(), 2, "array.sum.index");
  auto* total = body.CreatePHI(body.getInt64Ty(), 2, "array.sum.total");
  auto* bits = body.CreatePHI(body.getInt64Ty(), 2, "array.sum.bits");
  index->addIncoming(start, preheader);
  total->addIncoming(initial, preheader);
  bits->addIncoming(initial, preheader);
  auto* address = body.CreateGEP(reduction.address->getSourceElementType(),
                                  reduction.address->getPointerOperand(), index);
  auto* value = body.CreateAlignedLoad(body.getInt64Ty(), address, reduction.element->getAlign());
  value->setMetadata(llvm::LLVMContext::MD_tbaa,
                     reduction.element->getMetadata(llvm::LLVMContext::MD_tbaa));
  // 未验证时使用模加法；即使值超出窄域，丢弃的投机结果也不能产生 poison。
  auto* sum = body.CreateAdd(total, value);
  auto* combined = body.CreateOr(bits, value);
  auto* next = body.CreateNUWAdd(index, body.getInt64(1));
  auto* continued = body.CreateCondBr(body.CreateICmpNE(next, reduction.bound), trial, validate);
  index->addIncoming(next, trial);
  total->addIncoming(sum, trial);
  bits->addIncoming(combined, trial);
  llvm::Metadata* properties[] = {
      nullptr,
      llvm::MDNode::get(context, {llvm::MDString::get(context, "llvm.loop.vectorize.enable"),
                                 llvm::ConstantAsMetadata::get(body.getTrue())}),
      llvm::MDNode::get(context, {llvm::MDString::get(context, "llvm.loop.interleave.count"),
                                 llvm::ConstantAsMetadata::get(body.getInt32(kTrialInterleaveCount))}),
  };
  auto* metadata = llvm::MDNode::getDistinct(context, properties);
  metadata->replaceOperandWith(0, metadata);
  continued->setMetadata(llvm::LLVMContext::MD_loop, metadata);

  llvm::IRBuilder<> checking(validate);
  checking.CreateCondBr(checking.CreateICmpULE(combined, checking.getInt64(kSmallIntegerMask)),
                          commit, header);
  reduction.index->addIncoming(start, validate);
  reduction.total->addIncoming(initial, validate);
  llvm::IRBuilder<> committed(commit);
  // 总项数不超过 uint32 length 加一个初值；31 位非负项的全部 int64 前缀均安全。
  // 只在验证成功后合并无观察期间的根清空，原错误路径保持每次写入与清空的顺序。
  for (auto* clear : reduction.clears) {
    auto* copied = committed.CreateAlignedStore(clear->getValueOperand(), clear->getPointerOperand(),
                                                 clear->getAlign());
    copied->setMetadata(kNativeRootClearMetadata, clear->getMetadata(kNativeRootClearMetadata));
    copied->setMetadata(llvm::LLVMContext::MD_tbaa, clear->getMetadata(llvm::LLVMContext::MD_tbaa));
  }
  for (auto& phi : reduction.normal_exit->phis()) {
    auto* incoming = phi.getIncomingValueForBlock(loop.getLoopLatch());
    if (incoming == reduction.index) incoming = index;
    else if (incoming == reduction.total) incoming = total;
    else if (incoming == reduction.increment) incoming = next;
    else if (incoming == reduction.sum) incoming = sum;
    else if (incoming == reduction.element) incoming = value;
    phi.addIncoming(incoming, commit);
  }
  committed.CreateBr(reduction.normal_exit);
}

class ArrayReductionPass : public llvm::PassInfoMixin<ArrayReductionPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    if (!function.hasFnAttribute(kNativeArrayReductionCandidateAttribute)) {
      return llvm::PreservedAnalyses::all();
    }
    const auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    llvm::SmallVector<ArrayReduction, 4> reductions;
    for (auto* loop : loops.getLoopsInPreorder()) {
      if (auto reduction = MatchReduction(*loop)) {
        reductions.push_back(std::move(*reduction));
      }
    }
    for (const auto& reduction : reductions) {
      EmitTrial(reduction);
    }
    if (reductions.empty()) return llvm::PreservedAnalyses::all();
    analyses.invalidate(function, llvm::PreservedAnalyses::none());
    llvm::FunctionPassManager cleanup;
    cleanup.addPass(llvm::LoopSimplifyPass());
    cleanup.addPass(llvm::LCSSAPass());
    auto options = llvm::LoopVectorizeOptions().setVectorizeOnlyWhenForced(true)
                       .setInterleaveOnlyWhenForced(true);
    cleanup.addPass(llvm::LoopVectorizePass(options));
    cleanup.addPass(llvm::InstCombinePass());
    cleanup.addPass(llvm::SimplifyCFGPass());
    cleanup.addPass(llvm::DCEPass());
    cleanup.run(function, analyses);
    return llvm::PreservedAnalyses::none();
  }
};

}  // namespace

void AddArrayReductionPasses(llvm::FunctionPassManager& passes) {
  passes.addPass(llvm::LoopSimplifyPass());
  passes.addPass(llvm::LCSSAPass());
  passes.addPass(ArrayReductionPass());
  passes.addPass(llvm::LCSSAPass());
}

}  // namespace vela::compiler::native
