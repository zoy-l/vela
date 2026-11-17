#include "compiler/native/array_reduction_optimizer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Analysis.h>
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
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/LCSSA.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/LoopUtils.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>

#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/memory_regions.h"
#include "compiler/native/native_values.h"
#include "runtime/heap/array_storage.h"
#include "runtime/native/native_memory_layout.h"

namespace vela::compiler::native {
namespace {

constexpr std::uint64_t kTrialInstructions = 16;
constexpr std::uint32_t kTrialInterleaveCount = 4;
constexpr unsigned kInlineReductionValues = 16;

struct ReductionState final {
  llvm::Value* index = nullptr;
  llvm::Value* total = nullptr;
  llvm::Value* increment = nullptr;
  llvm::Value* sum = nullptr;
  llvm::Value* element = nullptr;
};

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
  llvm::Value* array = nullptr;
  llvm::Value* bound = nullptr;
  llvm::LoadInst* length = nullptr;
  llvm::BasicBlock* normal_exit = nullptr;
  llvm::SmallVector<llvm::StoreInst*, 4> stores;
  std::uint64_t limit = 0;

  [[nodiscard]] std::array<std::pair<llvm::Value*, llvm::Value*>, 5> ExitMapping(
      const ReductionState& replacement = {}) const {
    return std::to_array<std::pair<llvm::Value*, llvm::Value*>>({
        {index, replacement.index},
        {total, replacement.total},
        {increment, replacement.increment},
        {sum, replacement.sum},
        {element, replacement.element},
    });
  }
};

using ArrayLengths = llvm::DenseMap<llvm::Value*, llvm::SmallVector<llvm::LoadInst*, 2>>;

std::optional<ArrayReduction> MatchReductionPhis(ArrayReduction reduction) {
  auto& loop = *reduction.loop;
  std::size_t count = 0;
  for (auto& phi : loop.getHeader()->phis()) {
    ++count;
    if (!phi.getType()->isIntegerTy(std::numeric_limits<std::uint64_t>::digits) ||
        phi.getNumIncomingValues() != 2) {
      return std::nullopt;
    }
    auto* incoming = phi.getIncomingValueForBlock(loop.getLoopLatch());
    if (auto* increment = llvm::dyn_cast<llvm::BinaryOperator>(incoming);
        increment != nullptr && increment->getOpcode() == llvm::Instruction::Add &&
        increment->hasNoSignedWrap() && increment->getOperand(0) == &phi) {
      const auto* step = llvm::dyn_cast<llvm::ConstantInt>(increment->getOperand(1));
      if (step == nullptr || !step->isOne()) {
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
  auto* comparison =
      branch == nullptr ? nullptr : llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
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
    if (extract == nullptr || extract->getNumIndices() != 1 || extract->getIndices().front() != 1 ||
        reduction.overflow != nullptr) {
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

std::optional<ArrayReduction> MatchNativeArrayStorage(ArrayReduction reduction,
                                                      const ArrayLengths& lengths,
                                                      llvm::DominatorTree& dominators) {
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
  if (address == nullptr || address->getNumIndices() != 1 ||
      address->getOperand(1) != reduction.index) {
    return std::nullopt;
  }
  const auto& layout = element->getModule()->getDataLayout();
  const auto stride = layout.getTypeAllocSize(address->getSourceElementType());
  auto* data = llvm::dyn_cast<llvm::LoadInst>(address->getPointerOperand());
  if (stride.isScalable() || stride.getFixedValue() != sizeof(std::int64_t) || data == nullptr ||
      !data->isSimple() || !loop.isLoopInvariant(data)) {
    return std::nullopt;
  }
  std::int64_t data_offset = 0;
  auto* data_owner =
      llvm::GetPointerBaseWithConstantOffset(data->getPointerOperand(), data_offset, layout);
  using runtime::NativeMemoryLayout;
  // Elements 的未标记 int64 读取及同一数组的 data/uint32 length 共同证明完整读取范围；
  // 不能仅凭当前单次读取的越界检查推断其余投机读取也有效。
  if (std::cmp_not_equal(data_offset, NativeMemoryLayout::kArrayData)) {
    return std::nullopt;
  }
  const auto found = lengths.find(data_owner);
  if (found == lengths.end()) {
    return std::nullopt;
  }
  for (auto* length : found->second) {
    if (loop.isLoopInvariant(length) &&
        dominators.dominates(length, loop.getLoopPreheader()->getTerminator())) {
      reduction.length = length;
      break;
    }
  }
  if (reduction.length == nullptr) {
    return std::nullopt;
  }
  reduction.element = element;
  reduction.address = address;
  reduction.array = data_owner;
  return reduction;
}

bool CanCommitReductionStore(const llvm::StoreInst* store, const ArrayReduction& reduction) {
  const auto& loop = *reduction.loop;
  if (store == nullptr || !store->isSimple() || !loop.isLoopInvariant(store->getPointerOperand())) {
    return false;
  }
  const auto* constant = llvm::dyn_cast<llvm::Constant>(store->getValueOperand());
  const auto clear = constant != nullptr && constant->isNullValue() &&
                     store->getMetadata(kNativeRootClearMetadata) != nullptr &&
                     HasStorageTag(*store, MemoryRegion::Roots);
  const auto field = store->getValueOperand() == reduction.element &&
                     HasStorageTag(*store, MemoryRegion::FieldPayloads);
  const auto kind = constant != nullptr && HasStorageTag(*store, MemoryRegion::FieldKinds);
  // 整数归约提交原来的末项及其 kind；与浮点字段暂存不同，不限制为 Double 标签。
  // 无调用和轮询的成功片段只提交一次不变描述；失败路径保留原写入前缀。
  const auto state = loop.isLoopInvariant(store->getValueOperand()) &&
                     (HasStorageTag(*store, MemoryRegion::Execution) ||
                      HasStorageTag(*store, MemoryRegion::Roots));
  return clear || field || kind || state;
}

std::optional<ArrayReduction> MatchPureReductionBody(ArrayReduction reduction) {
  auto& loop = *reduction.loop;
  llvm::SmallPtrSet<const llvm::Value*, kInlineReductionValues> allowed{
      reduction.update,
      reduction.overflow,
      reduction.address,
      loop.getHeader()->getTerminator(),
      loop.getLoopLatch()->getTerminator(),
      llvm::cast<llvm::CondBrInst>(loop.getLoopLatch()->getTerminator())->getCondition(),
  };
  const auto exit_values = reduction.ExitMapping();
  for (const auto& value : exit_values) {
    allowed.insert(value.first);
  }
  std::uint64_t instructions = kTrialInstructions;
  for (auto* block : loop.blocks()) {
    instructions += block->size();
    for (auto& instruction : *block) {
      if (allowed.contains(&instruction)) {
        continue;
      }
      if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
          intrinsic != nullptr &&
          intrinsic->getIntrinsicID() == llvm::Intrinsic::experimental_noalias_scope_decl) {
        continue;
      }
      auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (!CanCommitReductionStore(store, reduction)) {
        return std::nullopt;
      }
      reduction.stores.push_back(store);
    }
  }
  const auto mapped = [&](const llvm::Value* value) {
    return loop.isLoopInvariant(value) || std::ranges::any_of(exit_values, [&](const auto& entry) {
             return entry.first == value;
           });
  };
  if (!std::ranges::all_of(reduction.normal_exit->phis(), [&](const auto& phi) {
        return mapped(phi.getIncomingValueForBlock(loop.getLoopLatch()));
      })) {
    return std::nullopt;
  }
  // 最坏路径先扫描再归约，两次遍历共同消费同一个免轮询预算。
  reduction.limit = NativePollBudget(instructions) / 2;
  if (reduction.limit == 0) {
    return std::nullopt;
  }
  return reduction;
}

std::optional<ArrayReduction> MatchReduction(llvm::Loop& loop,
                                             const ArrayLengths& lengths,
                                             llvm::DominatorTree& dominators) {
  if (loop.getBlocks().size() != 2 || !loop.getSubLoops().empty() ||
      loop.getLoopPreheader() == nullptr || loop.getLoopLatch() == nullptr ||
      llvm::findOptionMDForLoop(&loop, kNativeArrayReductionFallbackMetadata) != nullptr) {
    return std::nullopt;
  }
  auto reduction = MatchReductionPhis({.loop = &loop, .stores = {}});
  if (!reduction) {
    return std::nullopt;
  }
  reduction = MatchReductionExits(std::move(*reduction));
  if (!reduction) {
    return std::nullopt;
  }
  reduction = MatchNativeArrayStorage(std::move(*reduction), lengths, dominators);
  if (!reduction) {
    return std::nullopt;
  }
  return MatchPureReductionBody(std::move(*reduction));
}

enum class ReductionRange : std::uint8_t { Scan, Certified };

llvm::BasicBlock* EmitTrial(const ArrayReduction& reduction,
                            llvm::BasicBlock* incoming,
                            ReductionRange range) {
  auto& loop = *reduction.loop;
  auto* header = loop.getHeader();
  auto* function = header->getParent();
  auto& context = function->getContext();
  const auto certified = range == ReductionRange::Certified;
  auto* trial = llvm::BasicBlock::Create(
      context, certified ? "array.sum.certified" : "array.sum.trial", function, header);
  auto* validate = certified
                       ? nullptr
                       : llvm::BasicBlock::Create(context, "array.sum.validate", function, header);
  auto* commit = llvm::BasicBlock::Create(context, "array.sum.commit", function, header);
  auto* preheader = loop.getLoopPreheader();
  auto* start = reduction.index->getIncomingValueForBlock(preheader);
  auto* initial = reduction.total->getIncomingValueForBlock(preheader);

  llvm::IRBuilder<> body(trial);
  auto* index = body.CreatePHI(body.getInt64Ty(), 2, "array.sum.index");
  auto* total = body.CreatePHI(body.getInt64Ty(), 2, "array.sum.total");
  auto* bits = certified ? nullptr : body.CreatePHI(body.getInt64Ty(), 2, "array.sum.bits");
  index->addIncoming(start, incoming);
  total->addIncoming(initial, incoming);
  if (bits != nullptr) {
    bits->addIncoming(initial, incoming);
  }
  auto* address = body.CreateGEP(reduction.address->getSourceElementType(),
                                 reduction.address->getPointerOperand(), index);
  auto* value = body.CreateAlignedLoad(body.getInt64Ty(), address, reduction.element->getAlign());
  value->setMetadata(llvm::LLVMContext::MD_tbaa,
                     reduction.element->getMetadata(llvm::LLVMContext::MD_tbaa));
  // 未验证时使用模加法；即使值超出窄域，丢弃的投机结果也不能产生 poison。
  auto* sum = body.CreateAdd(total, value);
  llvm::Value* combined = nullptr;
  if (bits != nullptr) {
    combined = body.CreateOr(bits, value);
  }
  auto* next = body.CreateNUWAdd(index, body.getInt64(1));
  auto* continued = body.CreateCondBr(body.CreateICmpNE(next, reduction.bound), trial,
                                      certified ? commit : validate);
  index->addIncoming(next, trial);
  total->addIncoming(sum, trial);
  if (bits != nullptr) {
    bits->addIncoming(combined, trial);
  }
  std::array<llvm::Metadata*, 3> properties{
      nullptr,
      llvm::MDNode::get(context,
                        {
                            llvm::MDString::get(context, "llvm.loop.vectorize.enable"),
                            llvm::ConstantAsMetadata::get(body.getTrue()),
                        }),
      llvm::MDNode::get(context,
                        {
                            llvm::MDString::get(context, "llvm.loop.interleave.count"),
                            llvm::ConstantAsMetadata::get(body.getInt32(kTrialInterleaveCount)),
                        }),
  };
  auto* metadata = llvm::MDNode::getDistinct(context, properties);
  metadata->replaceOperandWith(0, metadata);
  continued->setMetadata(llvm::LLVMContext::MD_loop, metadata);

  if (validate != nullptr) {
    llvm::IRBuilder<> checking(validate);
    checking.CreateCondBr(
        checking.CreateICmpULE(combined, checking.getInt64(runtime::internal::kNonnegative31Mask)),
        commit, header);
    reduction.index->addIncoming(start, validate);
    reduction.total->addIncoming(initial, validate);
  }
  llvm::IRBuilder<> committed(commit);
  // 总项数不超过 uint32 length 加一个初值；31 位非负项的全部 int64 前缀均安全。
  // 只在全部前缀求和均安全后提交最终字段值和根清空；原错误路径保留逐项写入顺序。
  for (auto* store : reduction.stores) {
    auto* stored = store->getValueOperand() == reduction.element ? value : store->getValueOperand();
    auto* copied =
        committed.CreateAlignedStore(stored, store->getPointerOperand(), store->getAlign());
    copied->setMetadata(kNativeRootClearMetadata, store->getMetadata(kNativeRootClearMetadata));
    copied->setMetadata(llvm::LLVMContext::MD_tbaa, store->getMetadata(llvm::LLVMContext::MD_tbaa));
  }
  const auto exit_mapping = reduction.ExitMapping({
      .index = index,
      .total = total,
      .increment = next,
      .sum = sum,
      .element = value,
  });
  const std::span mapped(exit_mapping);
  for (auto& phi : reduction.normal_exit->phis()) {
    auto* value_on_exit = phi.getIncomingValueForBlock(loop.getLoopLatch());
    if (const auto found = std::ranges::find_if(
            mapped, [&](const auto& entry) { return entry.first == value_on_exit; });
        found != mapped.end()) {
      value_on_exit = found->second;
    }
    phi.addIncoming(value_on_exit, commit);
  }
  committed.CreateBr(reduction.normal_exit);
  return trial;
}

void EmitTrials(const ArrayReduction& reduction) {
  auto& loop = *reduction.loop;
  auto* header = loop.getHeader();
  auto* preheader = loop.getLoopPreheader();
  auto* old_entry = preheader->getTerminator();
  auto* function = header->getParent();
  auto* dispatch =
      llvm::BasicBlock::Create(function->getContext(), "array.sum.range", function, header);
  auto* start = reduction.index->getIncomingValueForBlock(preheader);
  auto* initial = reduction.total->getIncomingValueForBlock(preheader);
  // 在修改循环入口前完成两个克隆；匹配结果及原 PHI 的前驱仍然有效。
  auto* certified = EmitTrial(reduction, dispatch, ReductionRange::Certified);
  auto* scanned = EmitTrial(reduction, dispatch, ReductionRange::Scan);
  llvm::IRBuilder<> guard(old_entry);
  auto* distance = guard.CreateSub(reduction.bound, start);
  auto* nonempty = guard.CreateICmpULT(start, reduction.bound);
  auto* within_array =
      guard.CreateICmpULE(reduction.bound, guard.CreateZExt(reduction.length, guard.getInt64Ty()));
  auto* bounded = guard.CreateICmpULE(distance, guard.getInt64(reduction.limit));
  guard.CreateCondBr(guard.CreateAnd(guard.CreateAnd(nonempty, within_array), bounded), dispatch,
                     header);
  old_entry->eraseFromParent();
  llvm::IRBuilder<> select(dispatch);
  auto* narrow = HasNonnegative31ArrayRange(select, reduction.array);
  auto* safe_initial =
      select.CreateICmpULE(initial, select.getInt64(runtime::internal::kNonnegative31Mask));
  select.CreateCondBr(select.CreateAnd(narrow, safe_initial), certified, scanned);
}

class ArrayReductionPass : public llvm::PassInfoMixin<ArrayReductionPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    if (!function.hasFnAttribute(kNativeArrayReductionCandidateAttribute)) {
      return llvm::PreservedAnalyses::all();
    }
    const auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    ArrayLengths lengths;
    for (auto& instruction : llvm::instructions(function)) {
      auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr || !load->isSimple() ||
          !load->getType()->isIntegerTy(std::numeric_limits<std::uint32_t>::digits) ||
          !HasStorageTag(*load, MemoryRegion::HeapHeader)) {
        continue;
      }
      std::int64_t offset = 0;
      auto* owner = llvm::GetPointerBaseWithConstantOffset(load->getPointerOperand(), offset,
                                                           function.getParent()->getDataLayout());
      if (std::cmp_equal(offset, runtime::NativeMemoryLayout::kObjectLength)) {
        lengths[owner].push_back(load);
      }
    }
    llvm::SmallVector<ArrayReduction, 4> reductions;
    for (auto* loop : loops.getLoopsInPreorder()) {
      if (auto reduction = MatchReduction(*loop, lengths, dominators)) {
        reductions.push_back(std::move(*reduction));
      }
    }
    for (const auto& reduction : reductions) {
      EmitTrials(reduction);
      // 失败后的原循环已消费一次试算预算；后续流水线不得再向其叠加试算，
      // 否则未知范围会在一次安全点间隔内重复扫描并突破最大免轮询工作量。
      llvm::addStringMetadataToLoop(reduction.loop, kNativeArrayReductionFallbackMetadata, 1);
    }
    if (reductions.empty()) {
      return llvm::PreservedAnalyses::all();
    }
    analyses.invalidate(function, llvm::PreservedAnalyses::none());
    llvm::FunctionPassManager cleanup;
    cleanup.addPass(llvm::EarlyCSEPass(true));
    cleanup.addPass(llvm::SimplifyCFGPass());
    cleanup.addPass(llvm::LoopSimplifyPass());
    cleanup.addPass(llvm::LCSSAPass());
    auto options =
        llvm::LoopVectorizeOptions().setVectorizeOnlyWhenForced(true).setInterleaveOnlyWhenForced(
            true);
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
