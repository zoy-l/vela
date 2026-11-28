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
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Analysis/VectorUtils.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
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
#include "compiler/native/loop_hints.h"
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

bool HasConsecutiveLanes(llvm::Value* value,
                         llvm::SmallPtrSetImpl<llvm::Value*>& active,
                         bool& has_seed) {
  if (const auto* constant = llvm::dyn_cast<llvm::Constant>(value)) {
    const auto* type = llvm::dyn_cast<llvm::FixedVectorType>(value->getType());
    if (type == nullptr) {
      return false;
    }
    const auto* first =
        llvm::dyn_cast_or_null<llvm::ConstantInt>(constant->getAggregateElement(0U));
    if (first == nullptr) {
      return false;
    }
    for (unsigned lane = 1; lane < type->getNumElements(); ++lane) {
      const auto* item =
          llvm::dyn_cast_or_null<llvm::ConstantInt>(constant->getAggregateElement(lane));
      if (item == nullptr || item->getValue() != first->getValue() + lane) {
        return false;
      }
    }
    has_seed = true;
    return true;
  }
  if (auto* phi = llvm::dyn_cast<llvm::PHINode>(value)) {
    if (!active.insert(phi).second) {
      return true;
    }
    const auto valid = std::ranges::all_of(phi->incoming_values(), [&](auto& incoming) {
      return HasConsecutiveLanes(incoming.get(), active, has_seed);
    });
    active.erase(phi);
    return valid;
  }
  const auto* operation = llvm::dyn_cast<llvm::BinaryOperator>(value);
  if (operation == nullptr) {
    return false;
  }
  auto* left = operation->getOperand(0);
  auto* right = operation->getOperand(1);
  if ((operation->getOpcode() == llvm::Instruction::Add ||
       operation->getOpcode() == llvm::Instruction::Sub) &&
      llvm::getSplatValue(right) != nullptr) {
    return HasConsecutiveLanes(left, active, has_seed);
  }
  return operation->getOpcode() == llvm::Instruction::Add && llvm::getSplatValue(left) != nullptr &&
         HasConsecutiveLanes(right, active, has_seed);
}

struct ArrayLoopShape final {
  std::size_t instructions = 0;
  bool has_records = false;
  bool has_floating_element_writes = false;
  bool has_calls = false;
};

std::optional<ArrayLoopShape> AnalyzeArrayLoopShape(const llvm::Loop& loop) {
  ArrayLoopShape shape;
  bool has_elements = false;
  bool has_fields = false;
  for (const auto* block : loop.blocks()) {
    shape.instructions += block->size();
    for (const auto& instruction : *block) {
      const auto element = HasStorageTag(instruction, MemoryRegion::Elements);
      has_elements |= element;
      has_fields |= IsFloatingFieldStore(instruction);
      shape.has_records |= instruction.getMetadata(kNativeRecordFieldMetadata) != nullptr;
      shape.has_calls |= llvm::isa<llvm::CallBase>(instruction);
      if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction); store != nullptr) {
        shape.has_floating_element_writes |=
            element && store->isSimple() && store->getValueOperand()->getType()->isFPOrFPVectorTy();
      }
    }
  }
  return has_elements && !has_fields ? std::optional(shape) : std::nullopt;
}

class ArrayAccessUnrollHintsPass : public llvm::PassInfoMixin<ArrayAccessUnrollHintsPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    constexpr unsigned kPartialUnrollCount = 4;
    constexpr unsigned kRecordUnrollCount = 16;
    constexpr std::size_t kExpandedInstructionBudget = 512;
    const auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    bool changed = false;
    for (auto* loop : loops.getLoopsInPreorder()) {
      if (!loop->getSubLoops().empty() || HasLoopUnrollHints(*loop)) {
        continue;
      }
      const auto shape = AnalyzeArrayLoopShape(*loop);
      if (!shape) {
        continue;
      }
      const auto instructions = shape->instructions;
      auto& evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
      const auto trips = evolution.getSmallConstantMaxTripCount(loop);
      if (trips != 0 && trips <= kPartialUnrollCount) {
        continue;
      }
      // 无调用的单出口内层可以分摊连续更新的循环控制；轮询仍留在外层片段边界。
      const auto contiguous_update = shape->has_floating_element_writes && !shape->has_calls &&
                                     loop->getExitingBlock() != nullptr;
      const auto bounded_record = (shape->has_records || contiguous_update) && trips == 0 &&
                                  instructions <= kExpandedInstructionBudget / kPartialUnrollCount;
      const auto large_known = trips != 0 && instructions > kExpandedInstructionBudget / trips;
      if (bounded_record || large_known) {
        // 受查数组访问的冷错误块会随全展开一起复制；限制体积，保留每次访问的检查。
        auto unroll = kPartialUnrollCount;
        if (bounded_record) {
          while (unroll < kRecordUnrollCount &&
                 instructions <=
                     kExpandedInstructionBudget / (static_cast<std::size_t>(unroll) * 2)) {
            unroll *= 2;
          }
        }
        SetLoopUnrollCount(*loop, unroll, LoopUnrollHints::Retain);
        changed = true;
      }
    }
    return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
  }
};

class ConsecutiveArrayLoadPass : public llvm::PassInfoMixin<ConsecutiveArrayLoadPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    const auto& layout = function.getParent()->getDataLayout();
    bool changed = false;
    for (auto current = llvm::inst_begin(function); current != llvm::inst_end(function);) {
      auto* gather = llvm::dyn_cast<llvm::IntrinsicInst>(&*current++);
      if (gather == nullptr || gather->getIntrinsicID() != llvm::Intrinsic::masked_gather ||
          !HasStorageTag(*gather, MemoryRegion::Elements)) {
        continue;
      }
      const auto* mask = llvm::dyn_cast<llvm::Constant>(gather->getArgOperand(1));
      auto* address = llvm::dyn_cast<llvm::GetElementPtrInst>(gather->getArgOperand(0));
      auto* type = llvm::dyn_cast<llvm::FixedVectorType>(gather->getType());
      if (address == nullptr || address->getNumIndices() != 1 || !address->isInBounds() ||
          !address->getPointerOperand()->getType()->isPointerTy() || type == nullptr ||
          mask == nullptr || !mask->isAllOnesValue() ||
          layout.getTypeAllocSize(address->getSourceElementType()) !=
              layout.getTypeAllocSize(type->getElementType())) {
        continue;
      }
      auto* indices = address->getOperand(1);
      // 窄索引的回绕会跨越 GEP 的符号扩展边界，不能据其模整数 lane 差推导连续地址。
      if (indices->getType()->getScalarSizeInBits() !=
          layout.getIndexSizeInBits(address->getPointerAddressSpace())) {
        continue;
      }
      constexpr unsigned kInlineGatherProvenanceValues = 8;
      llvm::SmallPtrSet<llvm::Value*, kInlineGatherProvenanceValues> active;
      bool has_seed = false;
      if (!HasConsecutiveLanes(indices, active, has_seed) || !has_seed) {
        continue;
      }
      // 每个 PHI 入口及回边都保持相邻 lane 差为一；全有效 mask 和 inbounds 证明
      // 这一整段均可读。恢复连续加载，避免 LLVM 对 byte 数组 GEP 选择硬件 gather。
      llvm::IRBuilder<> builder(gather);
      auto* first = builder.CreateExtractElement(indices, builder.getInt32(0));
      auto* pointer = builder.CreateInBoundsGEP(address->getSourceElementType(),
                                                address->getPointerOperand(), first);
      auto* loaded =
          builder.CreateAlignedLoad(type, pointer, gather->getParamAlign(0).valueOrOne());
      loaded->copyMetadata(*gather);
      gather->replaceAllUsesWith(loaded);
      gather->eraseFromParent();
      changed = true;
    }
    if (!changed) {
      return llvm::PreservedAnalyses::all();
    }
    analyses.invalidate(function, llvm::PreservedAnalyses::none());
    llvm::FunctionPassManager cleanup;
    cleanup.addPass(llvm::InstCombinePass());
    cleanup.addPass(llvm::DCEPass());
    cleanup.run(function, analyses);
    return llvm::PreservedAnalyses::none();
  }
};

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
  passes.addPass(ConsecutiveArrayLoadPass());
  passes.addPass(ArrayAccessUnrollHintsPass());
  passes.addPass(llvm::LCSSAPass());
}

}  // namespace vela::compiler::native
