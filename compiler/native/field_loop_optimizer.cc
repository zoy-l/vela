#include "compiler/native/field_loop_optimizer.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/Loads.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/DeadStoreElimination.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/LICM.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Transforms/Scalar/LoopUnrollPass.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/LCSSA.h>
#include <llvm/Transforms/Utils/LoopPeel.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <llvm/Transforms/Vectorize/SLPVectorizer.h>

#include "compiler/native/array_reduction_optimizer.h"
#include "compiler/native/integer_loop_optimizer.h"
#include "compiler/native/loop_cloning.h"
#include "compiler/native/loop_hints.h"
#include "compiler/native/memory_regions.h"
#include "runtime/native/native_abi.h"

namespace vela::compiler::native {
namespace {

constexpr unsigned kMaximumFieldLoopTrips = 8;
constexpr unsigned kMaximumNestedUnrollRounds = 16;
constexpr std::size_t kMaximumFieldLoopInstructions = 4096;
constexpr std::size_t kMaximumFieldLocations = 64;
constexpr unsigned kMaximumFieldVectorLanes = 8;
constexpr unsigned kInlineFieldLocations = 16;
constexpr unsigned kInlineFieldAccesses = 8;

bool IsFieldLoopCandidate(const llvm::Function& function) {
  return function.hasFnAttribute(kNativeArrayReductionCandidateAttribute) &&
         std::ranges::any_of(llvm::instructions(function), IsFloatingFieldStore);
}

struct FieldLoopCost final {
  std::size_t instructions = 0;
  bool fields = false;
  bool normal_calls = false;
};

FieldLoopCost MeasureFieldLoop(const llvm::Loop& loop,
                               const llvm::SmallPtrSetImpl<const llvm::BasicBlock*>& failures) {
  FieldLoopCost cost;
  // 体积预算计入完整副本。调用资格只检查守卫成功且无请求时可达的块。
  for (const auto* block : loop.blocks()) {
    cost.instructions += block->size();
  }
  llvm::SmallVector<llvm::BasicBlock*, kInlineFieldLocations> pending{loop.getHeader()};
  llvm::SmallPtrSet<llvm::BasicBlock*, kInlineFieldLocations> visited;
  while (!pending.empty()) {
    auto* block = pending.pop_back_val();
    if (!loop.contains(block) || failures.contains(block) || !visited.insert(block).second) {
      continue;
    }
    for (const auto& instruction : *block) {
      cost.fields |= IsFloatingFieldStore(instruction);
      if (llvm::isa<llvm::CallBase>(instruction) && !llvm::isa<llvm::IntrinsicInst>(instruction)) {
        cost.normal_calls = true;
        return cost;
      }
    }
    auto* branch = llvm::dyn_cast<llvm::CondBrInst>(block->getTerminator());
    if (const auto ready = branch == nullptr ? std::nullopt : FindReadyPollEdge(*branch)) {
      pending.push_back(branch->getSuccessor(*ready));
    } else {
      pending.append(llvm::succ_begin(block), llvm::succ_end(block));
    }
  }
  return cost;
}

class FieldLoopHintsPass : public llvm::PassInfoMixin<FieldLoopHintsPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    const auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
    const auto function_size = function.getInstructionCount();
    llvm::SmallPtrSet<const llvm::BasicBlock*, kInlineFieldLocations> failures;
    FindGuardFailureBlocks(function, failures);
    for (auto* loop : loops.getLoopsInPreorder()) {
      const auto trips = evolution.getSmallConstantMaxTripCount(loop);
      if (trips == 0 || trips > kMaximumFieldLoopTrips) {
        continue;
      }
      const auto cost = MeasureFieldLoop(*loop, failures);
      const auto instructions = cost.instructions;
      if (!cost.fields || cost.normal_calls ||
          instructions * trips > kMaximumFieldLoopInstructions ||
          function_size + (instructions * (trips - 1)) > 4 * kMaximumFieldLoopInstructions) {
        continue;
      }
      if (trips == 1) {
        auto* latch = loop->getLoopLatch();
        auto* branch =
            latch == nullptr ? nullptr : llvm::dyn_cast<llvm::CondBrInst>(latch->getTerminator());
        if (branch == nullptr ||
            (loop->contains(branch->getSuccessor(0)) == loop->contains(branch->getSuccessor(1)))) {
          continue;
        }
        branch->setCondition(llvm::ConstantInt::getBool(function.getContext(),
                                                        !loop->contains(branch->getSuccessor(0))));
        return llvm::PreservedAnalyses::none();
      }
      SetLoopUnrollCount(*loop, trips, LoopUnrollHints::Replace);
      return llvm::PreservedAnalyses::none();
    }
    return llvm::PreservedAnalyses::all();
  }
};

struct FieldLocation final {
  llvm::Value* pointer;
  llvm::SmallVector<llvm::Instruction*, kInlineFieldAccesses> accesses;
  llvm::Instruction* first = nullptr;
};

bool IsScalarizableFieldAccess(const llvm::Instruction& instruction) {
  llvm::Type* type = nullptr;
  if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    if (!load->isSimple()) {
      return false;
    }
    type = load->getType();
  } else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
    if (!store->isSimple()) {
      return false;
    }
    type = store->getValueOperand()->getType();
  }
  const auto* vector = llvm::dyn_cast_or_null<llvm::FixedVectorType>(type);
  return vector != nullptr && vector->getElementType()->isDoubleTy() &&
         vector->getNumElements() <= kMaximumFieldVectorLanes &&
         HasStorageTag(instruction, MemoryRegion::FieldPayloads);
}

void ScalarizeFieldAccess(llvm::Instruction& access) {
  auto* load = llvm::dyn_cast<llvm::LoadInst>(&access);
  auto* store = llvm::dyn_cast<llvm::StoreInst>(&access);
  auto* type = llvm::cast<llvm::FixedVectorType>(
      load != nullptr ? load->getType() : store->getValueOperand()->getType());
  auto* pointer = load != nullptr ? load->getPointerOperand() : store->getPointerOperand();
  const auto alignment = load != nullptr ? load->getAlign() : store->getAlign();
  llvm::IRBuilder<> builder(&access);
  llvm::Value* value = load != nullptr ? llvm::PoisonValue::get(type) : store->getValueOperand();
  for (unsigned lane = 0; lane < type->getNumElements(); ++lane) {
    const auto offset = lane * sizeof(double);
    auto* address = builder.CreateConstGEP1_64(builder.getInt8Ty(), pointer, offset);
    llvm::Instruction* scalar = nullptr;
    if (load != nullptr) {
      auto* item = builder.CreateAlignedLoad(builder.getDoubleTy(), address,
                                             llvm::commonAlignment(alignment, offset));
      scalar = item;
      value = builder.CreateInsertElement(value, item, lane);
    } else {
      scalar = builder.CreateAlignedStore(builder.CreateExtractElement(value, lane), address,
                                          llvm::commonAlignment(alignment, offset));
    }
    scalar->copyMetadata(access);
  }
  if (load != nullptr) {
    load->replaceAllUsesWith(value);
  }
  access.eraseFromParent();
}

class ScalarFieldAccessPass : public llvm::PassInfoMixin<ScalarFieldAccessPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& /*analyses*/) {
    llvm::SmallVector<llvm::Instruction*, kInlineFieldLocations> accesses;
    for (auto& instruction : llvm::instructions(function)) {
      if (IsScalarizableFieldAccess(instruction)) {
        accesses.push_back(&instruction);
      }
    }
    for (auto* access : accesses) {
      ScalarizeFieldAccess(*access);
    }
    return accesses.empty() ? llvm::PreservedAnalyses::all() : llvm::PreservedAnalyses::none();
  }
};

struct FieldLoopPlan final {
  llvm::Loop* loop;
  llvm::SmallVector<llvm::BasicBlock*, kInlineFieldLocations> blocks;
  llvm::SmallVector<FieldLocation, kInlineFieldLocations> fields;
  llvm::SmallVector<FieldLocation, kInlineFieldAccesses> readers;
  llvm::SmallVector<std::pair<llvm::Value*, llvm::Value*>, kInlineFieldLocations> disjoint;
};

struct FieldGuard final {
  llvm::CondBrInst* branch;
  unsigned success_edge;
};

using FieldGuards = llvm::SmallVector<FieldGuard, kInlineFieldLocations>;

llvm::MemoryLocation Location(const llvm::Instruction* instruction) {
  if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(instruction)) {
    return llvm::MemoryLocation::get(load);
  }
  return llvm::MemoryLocation::get(llvm::cast<llvm::StoreInst>(instruction));
}

bool IsEntryInvariant(const llvm::Value* pointer,
                      const llvm::Loop& loop,
                      const llvm::DominatorTree& dominators) {
  const auto* definition = llvm::dyn_cast<llvm::Instruction>(pointer);
  return loop.isLoopInvariant(pointer) &&
         (definition == nullptr ||
          dominators.dominates(definition, loop.getLoopPreheader()->getTerminator()));
}

bool IsAdministrative(const llvm::StoreInst& store) {
  // 此阶段只暂存 double 字段载荷；其他 kind 写入仍是可观察内存效果，不能一并折叠。
  const auto* constant = llvm::dyn_cast<llvm::Constant>(store.getValueOperand());
  if (constant == nullptr) {
    return false;
  }
  if (constant->isNullValue() && HasStorageTag(store, MemoryRegion::Roots)) {
    return true;
  }
  const auto* kind = llvm::dyn_cast<llvm::ConstantInt>(constant);
  return kind != nullptr &&
         kind->equalsInt(static_cast<unsigned>(runtime::NativeValueKind::Double)) &&
         HasStorageTag(store, MemoryRegion::FieldKinds);
}

bool CanStageInstruction(const llvm::Instruction& instruction) {
  if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    return load->isSimple();
  }
  if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
    return store->isSimple();
  }
  if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
    return llvm::isa<llvm::AssumeInst>(call) ||
           (llvm::isa<llvm::IntrinsicInst>(call) && call->doesNotAccessMemory() &&
            !call->mayThrow() && call->willReturn() && !call->isConvergent());
  }
  return !instruction.mayReadOrWriteMemory() && !instruction.mayHaveSideEffects();
}

bool CanStageMemory(const llvm::Loop& loop) {
  return std::ranges::all_of(loop.blocks(), [](const auto* block) {
    return llvm::isa<llvm::UncondBrInst, llvm::CondBrInst>(block->getTerminator()) &&
           std::ranges::all_of(*block, CanStageInstruction);
  });
}

using ExecutedFieldAccesses = llvm::DenseMap<const llvm::Value*, llvm::Align>;

void RecordExecutedFieldAccess(const llvm::Instruction& instruction, ExecutedFieldAccesses& seen) {
  if (!HasStorageTag(instruction, MemoryRegion::FieldPayloads)) {
    return;
  }
  const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
  const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
  if ((load == nullptr || !load->getType()->isDoubleTy()) &&
      (store == nullptr || !store->getValueOperand()->getType()->isDoubleTy())) {
    return;
  }
  const auto* pointer = load != nullptr ? load->getPointerOperand() : store->getPointerOperand();
  const auto alignment = load != nullptr ? load->getAlign() : store->getAlign();
  auto& prior = seen[pointer];
  prior = std::max(prior, alignment);
}

ExecutedFieldAccesses FindExecutedFieldAccesses(const llvm::BasicBlock& preheader) {
  ExecutedFieldAccesses seen;
  std::size_t inspected = 0;
  // 沿已执行的唯一前驱回溯首轮，只采用可能改变存储生命周期的操作之后的访问。
  for (const auto* block = &preheader; block != nullptr; block = block->getSinglePredecessor()) {
    if (!llvm::isa<llvm::UncondBrInst, llvm::CondBrInst>(block->getTerminator())) {
      break;
    }
    for (const auto& instruction : llvm::reverse(*block)) {
      if (++inspected > kMaximumFieldLoopInstructions) {
        return seen;
      }
      if (instruction.isTerminator()) {
        continue;
      }
      if (!CanStageInstruction(instruction)) {
        return seen;
      }
      RecordExecutedFieldAccess(instruction, seen);
    }
  }
  return seen;
}

bool CanSpeculateFieldInstruction(const llvm::Instruction& instruction,
                                  const ExecutedFieldAccesses& seen) {
  if (instruction.isTerminator() || llvm::isa<llvm::PHINode>(instruction)) {
    return true;
  }
  if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    const auto found = seen.find(load->getPointerOperand());
    return load->getType()->isDoubleTy() && HasStorageTag(*load, MemoryRegion::FieldPayloads) &&
           found != seen.end() && found->second >= load->getAlign() &&
           !llvm::mustSuppressSpeculation(*load);
  }
  return llvm::isSafeToSpeculativelyExecute(&instruction);
}

std::optional<FieldGuard> MatchFieldGuard(llvm::CondBrInst& branch, const llvm::Loop& loop) {
  const auto* compare = llvm::dyn_cast<llvm::FCmpInst>(branch.getCondition());
  const auto success = static_cast<unsigned>(!loop.contains(branch.getSuccessor(0)));
  const auto expected = success == 0 ? llvm::CmpInst::FCMP_UNE : llvm::CmpInst::FCMP_OEQ;
  if (compare == nullptr || compare->getPredicate() != expected ||
      !compare->getOperand(0)->getType()->isDoubleTy()) {
    return std::nullopt;
  }
  const auto* zero = llvm::dyn_cast<llvm::ConstantFP>(compare->getOperand(1));
  if (zero == nullptr || !zero->isZero()) {
    return std::nullopt;
  }
  return FieldGuard{.branch = &branch, .success_edge = success};
}

FieldGuards FindFieldGuardBatch(const FieldLoopPlan& plan) {
  const auto& loop = *plan.loop;
  const auto* latch = loop.getLoopLatch();
  FieldGuards guards;
  for (auto* block : plan.blocks) {
    auto* branch = llvm::dyn_cast<llvm::CondBrInst>(block->getTerminator());
    if (block == latch || branch == nullptr) {
      continue;
    }
    const auto guard = MatchFieldGuard(*branch, loop);
    if (!guard) {
      return {};
    }
    guards.push_back(*guard);
  }
  if (guards.size() < 2) {
    return {};
  }
  const auto seen = FindExecutedFieldAccesses(*loop.getLoopPreheader());
  const auto* first_guard = guards.front().branch->getParent();
  bool speculative = false;
  for (const auto* block : plan.blocks) {
    if (!std::ranges::all_of(*block, [&](const auto& instruction) {
          if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
            // 只有已纳入暂存计划的写入能在失败后丢弃，包括标签和根槽在内的其他写入都不行。
            return !IsAdministrative(*store);
          }
          return (!speculative && llvm::isa<llvm::LoadInst>(instruction)) ||
                 CanSpeculateFieldInstruction(instruction, seen);
        })) {
      return {};
    }
    speculative |= block == first_guard;
  }
  return guards;
}

bool HasStablePointer(const llvm::Value* pointer,
                      const llvm::Loop& loop,
                      llvm::AAResults& aliases,
                      llvm::ArrayRef<llvm::MemoryLocation> stores,
                      llvm::DenseMap<const llvm::Value*, bool>& stable) {
  if (loop.isLoopInvariant(pointer)) {
    return true;
  }
  if (const auto found = stable.find(pointer); found != stable.end()) {
    return found->second;
  }
  bool result = false;
  if (const auto* address = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer)) {
    result = HasStablePointer(address->getPointerOperand(), loop, aliases, stores, stable) &&
             std::ranges::all_of(address->indices(), [&](const auto& index) {
               return loop.isLoopInvariant(index.get());
             });
  } else if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(pointer);
             load != nullptr && load->isSimple() && load->getType()->isPointerTy() &&
             loop.isLoopInvariant(load->getPointerOperand()) &&
             HasStorageTag(*load, MemoryRegion::Elements)) {
    const auto location = llvm::MemoryLocation::get(load);
    result = std::ranges::all_of(
        stores, [&](const auto& store) { return aliases.isNoAlias(location, store); });
  }
  stable.try_emplace(pointer, result);
  return result;
}

std::optional<llvm::SmallVector<llvm::LoadInst*, kInlineFieldAccesses>> MatchFieldEntry(
    const llvm::Loop& loop,
    const llvm::DominatorTree& dominators,
    llvm::AAResults& aliases) {
  std::size_t instructions = 0;
  bool needs_entry = false;
  llvm::SmallVector<llvm::MemoryLocation, kInlineFieldLocations> stores;
  llvm::SmallVector<const llvm::Value*, kInlineFieldLocations> fields;
  llvm::SmallVector<llvm::LoadInst*, kInlineFieldAccesses> loads;
  for (auto* block : loop.blocks()) {
    instructions += block->size();
    if (instructions > kMaximumFieldLoopInstructions) {
      return std::nullopt;
    }
    for (auto& instruction : *block) {
      const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (store != nullptr) {
        stores.push_back(llvm::MemoryLocation::get(store));
        if (IsAdministrative(*store)) {
          continue;
        }
        const auto* pointer = store->getPointerOperand();
        if (!store->getValueOperand()->getType()->isDoubleTy() ||
            !HasStorageTag(*store, MemoryRegion::FieldPayloads)) {
          return std::nullopt;
        }
        fields.push_back(pointer);
        needs_entry |= !IsEntryInvariant(pointer, loop, dominators);
      } else if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
                 load != nullptr && load->getType()->isPointerTy()) {
        loads.push_back(load);
      }
    }
  }
  if (!needs_entry) {
    return std::nullopt;
  }
  llvm::DenseMap<const llvm::Value*, bool> stable;
  if (!std::ranges::all_of(fields, [&](const auto* pointer) {
        return HasStablePointer(pointer, loop, aliases, stores, stable);
      })) {
    return std::nullopt;
  }
  llvm::erase_if(loads, [&](const auto* load) {
    return !HasStablePointer(load, loop, aliases, stores, stable);
  });
  return loads;
}

class FieldEntryPeelPass : public llvm::PassInfoMixin<FieldEntryPeelPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
    auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    auto& aliases = analyses.getResult<llvm::AAManager>(function);
    for (auto* loop : loops.getLoopsInPreorder()) {
      if (!loop->getSubLoops().empty() || !CanStageMemory(*loop) || !llvm::canPeel(loop)) {
        continue;
      }
      const auto stable = MatchFieldEntry(*loop, dominators, aliases);
      if (!stable) {
        continue;
      }
      auto& assumptions = analyses.getResult<llvm::AssumptionAnalysis>(function);
      llvm::ValueToValueMapTy mapped;
      // 首轮仍在原守卫后读取；完整成功后，剩余循环才能复用已经可用的固定数组元素。
      llvm::peelLoop(loop, 1, false, &loops, &evolution, dominators, &assumptions, true, mapped);
      const auto* entry = loop->getLoopPreheader()->getTerminator();
      for (auto* load : *stable) {
        llvm::Value* value = mapped.lookup(load);
        const auto* definition = llvm::dyn_cast_or_null<llvm::Instruction>(value);
        if (value != nullptr &&
            (definition == nullptr || dominators.dominates(definition, entry))) {
          // 借用成功首轮的真实读取，不在原守卫前额外解引用。
          load->replaceAllUsesWith(value);
          load->eraseFromParent();
        }
      }
      return llvm::PreservedAnalyses::none();
    }
    return llvm::PreservedAnalyses::all();
  }
};

llvm::SmallVector<llvm::BasicBlock*, kInlineFieldLocations> IterationPath(llvm::Loop& loop) {
  auto* latch = loop.getLoopLatch();
  llvm::SmallVector<llvm::BasicBlock*, kInlineFieldLocations> blocks;
  auto* current = loop.getHeader();
  while (true) {
    if (blocks.size() == loop.getBlocks().size()) {
      return {};
    }
    blocks.push_back(current);
    if (current == latch) {
      break;
    }
    llvm::BasicBlock* next = nullptr;
    for (auto* successor : llvm::successors(current)) {
      if (loop.contains(successor)) {
        if (next != nullptr || successor == loop.getHeader()) {
          return {};
        }
        next = successor;
      }
    }
    if (next == nullptr) {
      return {};
    }
    current = next;
  }
  if (blocks.size() != loop.getBlocks().size()) {
    return {};
  }
  return blocks;
}

struct FieldWrites final {
  llvm::SmallVector<FieldLocation, kInlineFieldLocations> fields;
  llvm::DenseMap<llvm::Value*, std::size_t> indices;
  std::size_t instructions = 0;
};

std::optional<FieldWrites> MatchFieldWrites(const llvm::Loop& loop,
                                            const llvm::DominatorTree& dominators) {
  FieldWrites result;
  std::size_t writes = 0;
  const auto* latch = loop.getLoopLatch()->getTerminator();
  for (auto* block : loop.blocks()) {
    result.instructions += block->size();
    if (result.instructions > kMaximumFieldLoopInstructions) {
      return std::nullopt;
    }
    for (auto& instruction : *block) {
      auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (store == nullptr || IsAdministrative(*store)) {
        continue;
      }
      auto* pointer = store->getPointerOperand();
      if (!IsEntryInvariant(pointer, loop, dominators) || !dominators.dominates(store, latch) ||
          !store->getValueOperand()->getType()->isDoubleTy() ||
          store->getAlign().value() < alignof(double) ||
          !HasStorageTag(*store, MemoryRegion::FieldPayloads)) {
        return std::nullopt;
      }
      auto [position, inserted] = result.indices.try_emplace(pointer, result.fields.size());
      if (inserted) {
        result.fields.push_back({.pointer = pointer, .accesses = {}});
      }
      result.fields[position->second].accesses.push_back(store);
      ++writes;
    }
  }
  const auto count = result.fields.size();
  if (count == 0 || count >= writes || count > kMaximumFieldLocations) {
    return std::nullopt;
  }
  return result;
}

std::optional<llvm::SmallVector<FieldLocation, kInlineFieldAccesses>> MatchFieldReaders(
    const llvm::Loop& loop,
    FieldWrites& writes,
    const llvm::DominatorTree& dominators,
    llvm::AAResults& aliases) {
  llvm::SmallVector<FieldLocation, kInlineFieldAccesses> readers;
  llvm::DenseMap<llvm::Value*, std::size_t> indices;
  llvm::SmallVector<llvm::MemoryLocation, kInlineFieldLocations> locations;
  for (const auto& field : writes.fields) {
    locations.push_back(Location(field.accesses.front()));
  }
  for (auto* block : loop.blocks()) {
    for (auto& instruction : *block) {
      auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr) {
        continue;
      }
      auto* pointer = load->getPointerOperand();
      if (const auto found = writes.indices.find(pointer); found != writes.indices.end()) {
        if (!load->getType()->isDoubleTy()) {
          return std::nullopt;
        }
        writes.fields[found->second].accesses.push_back(load);
        continue;
      }
      const auto location = llvm::MemoryLocation::get(load);
      const auto overlaps = std::ranges::any_of(
          locations, [&](const auto& field) { return !aliases.isNoAlias(location, field); });
      if (!overlaps) {
        continue;
      }
      if (!load->getType()->isDoubleTy() || load->getAlign().value() < alignof(double) ||
          !IsEntryInvariant(pointer, loop, dominators)) {
        return std::nullopt;
      }
      auto [position, inserted] = indices.try_emplace(pointer, readers.size());
      if (inserted) {
        readers.push_back({.pointer = pointer, .accesses = {}});
      }
      readers[position->second].accesses.push_back(load);
    }
  }
  return readers;
}

llvm::Instruction* FirstDominatingAccess(const FieldLocation& field,
                                         const llvm::Loop& loop,
                                         const llvm::DominatorTree& dominators) {
  const auto* latch = loop.getLoopLatch()->getTerminator();
  auto* first = field.accesses.front();
  for (auto* access : field.accesses) {
    if (first != access && !dominators.dominates(first, access)) {
      first = access;
    }
  }
  // 指令级 dominates 判断值在使用点是否可用；自身位置需要单独接受。
  return dominators.dominates(first, latch) &&
                 std::ranges::all_of(field.accesses,
                                     [&](auto* access) {
                                       return first == access ||
                                              dominators.dominates(first, access);
                                     })
             ? first
             : nullptr;
}

void FindFieldDisjointness(FieldLoopPlan& plan, llvm::AAResults& aliases) {
  const auto& fields = plan.fields;
  for (std::size_t left = 0; left < fields.size(); ++left) {
    const auto& field = fields[left];
    const auto location = Location(field.accesses.front());
    const auto append = [&](const FieldLocation& right) {
      if (!aliases.isNoAlias(location, Location(right.accesses.front()))) {
        plan.disjoint.emplace_back(field.pointer, right.pointer);
      }
    };
    for (std::size_t right = left + 1; right < fields.size(); ++right) {
      append(fields[right]);
    }
    for (const auto& reader : plan.readers) {
      append(reader);
    }
  }
}

std::optional<FieldLoopPlan> AnalyzeFieldLoop(llvm::Loop& loop,
                                              llvm::DominatorTree& dominators,
                                              llvm::AAResults& aliases) {
  auto* preheader = loop.getLoopPreheader();
  auto* latch = loop.getLoopLatch();
  if (preheader == nullptr || latch == nullptr || !loop.getSubLoops().empty() ||
      !loop.isLoopExiting(latch) || !CanStageMemory(loop)) {
    return std::nullopt;
  }
  auto blocks = IterationPath(loop);
  if (blocks.empty()) {
    return std::nullopt;
  }
  auto writes = MatchFieldWrites(loop, dominators);
  if (!writes) {
    return std::nullopt;
  }
  auto readers = MatchFieldReaders(loop, *writes, dominators, aliases);
  if (!readers || writes->fields.size() + readers->size() > kMaximumFieldLocations) {
    return std::nullopt;
  }
  FieldLoopPlan plan{
      .loop = &loop,
      .blocks = std::move(blocks),
      .fields = std::move(writes->fields),
      .readers = std::move(*readers),
      .disjoint = {},
  };
  for (auto& field : plan.fields) {
    field.first = FirstDominatingAccess(field, loop, dominators);
    if (field.first == nullptr) {
      return std::nullopt;
    }
  }
  FindFieldDisjointness(plan, aliases);
  std::size_t exits = 0;
  for (auto* block : plan.blocks) {
    if (block == latch) {
      continue;
    }
    for (auto* successor : llvm::successors(block)) {
      exits += static_cast<std::size_t>(!loop.contains(successor));
    }
  }
  // 预算覆盖首轮剥离、克隆、别名守卫及所有失败出口的最坏前缀提交。
  const auto expansion = (2 * writes->instructions) + (4 * plan.disjoint.size()) +
                         (2 * plan.fields.size() * (exits + 2)) + exits;
  if (expansion > kMaximumFieldLoopInstructions) {
    return std::nullopt;
  }
  return plan;
}

void AddScope(llvm::Instruction& instruction,
              unsigned kind,
              llvm::ArrayRef<llvm::Metadata*> values) {
  llvm::SmallVector<llvm::Metadata*, kInlineFieldLocations> combined;
  if (auto* existing = instruction.getMetadata(kind)) {
    for (const auto& operand : existing->operands()) {
      combined.push_back(operand.get());
    }
  }
  combined.append(values.begin(), values.end());
  instruction.setMetadata(kind, llvm::MDNode::get(instruction.getContext(), combined));
}

struct FieldStaging final {
  llvm::SmallVector<llvm::AllocaInst*, kInlineFieldLocations> shadows;
  llvm::DenseMap<llvm::Instruction*, std::size_t> written_fields;
};

template <class Annotate>
FieldStaging StageFieldAccesses(const FieldLoopPlan& plan,
                                const llvm::ValueToValueMapTy& mapped,
                                const Annotate& annotate) {
  auto* function = plan.loop->getHeader()->getParent();
  llvm::IRBuilder<> allocation(&function->getEntryBlock(), function->getEntryBlock().begin());
  FieldStaging staged;
  for (std::size_t index = 0; index < plan.fields.size(); ++index) {
    const auto& field = plan.fields[index];
    auto* shadow = allocation.CreateAlloca(allocation.getDoubleTy(), nullptr, "field.shadow");
    const auto alignment = shadow->getAlign();
    staged.shadows.push_back(shadow);
    for (auto* access : field.accesses) {
      auto* cloned = llvm::cast<llvm::Instruction>(mapped.lookup(access));
      annotate(*cloned, index);
      if (auto* load = llvm::dyn_cast<llvm::LoadInst>(cloned)) {
        if (access == field.first) {
          // 第一次读取保留原有守卫和位置，不能为了快路径在失败之前额外解引用。
          llvm::IRBuilder<> initialize(load->getNextNode());
          initialize.CreateStore(load, shadow);
        } else {
          load->setOperand(0, shadow);
          load->setAlignment(alignment);
        }
      } else {
        auto* store = llvm::cast<llvm::StoreInst>(cloned);
        store->setOperand(1, shadow);
        store->setAlignment(alignment);
        staged.written_fields.insert({access, index});
      }
    }
  }
  return staged;
}

template <class Flush>
void FlushFieldExitPrefixes(const FieldLoopPlan& plan,
                            const llvm::ValueToValueMapTy& mapped,
                            const FieldStaging& staged,
                            const Flush& flush_shadow) {
  auto& loop = *plan.loop;
  auto* function = loop.getHeader()->getParent();
  auto& context = function->getContext();
  llvm::SmallVector<bool, kInlineFieldLocations> written(plan.fields.size(), false);
  for (auto* block : plan.blocks) {
    for (auto& instruction : *block) {
      if (const auto found = staged.written_fields.find(&instruction);
          found != staged.written_fields.end()) {
        written[found->second] = true;
      }
    }
    if (block == loop.getLoopLatch() ||
        std::ranges::none_of(written, [](auto value) { return value; })) {
      continue;
    }
    auto* cloned = llvm::cast<llvm::BasicBlock>(mapped.lookup(block));
    auto* branch = cloned->getTerminator();
    for (unsigned edge = 0; edge < branch->getNumSuccessors(); ++edge) {
      auto* successor = block->getTerminator()->getSuccessor(edge);
      if (loop.contains(successor)) {
        continue;
      }
      auto* flush = llvm::BasicBlock::Create(context, "fields.prefix", function, successor);
      llvm::IRBuilder<> prefix(flush);
      // 唯一正常路径明确每个失败边之前已执行的写入；只提交该前缀，不重放计算。
      for (std::size_t index = 0; index < plan.fields.size(); ++index) {
        if (written[index]) {
          flush_shadow(prefix, index);
        }
      }
      prefix.CreateBr(successor);
      for (auto& phi : successor->phis()) {
        phi.setIncomingBlock(static_cast<unsigned>(phi.getBasicBlockIndex(cloned)), flush);
      }
      branch->setSuccessor(edge, flush);
    }
  }
}

llvm::Value* BuildFieldGuardCheck(llvm::ArrayRef<llvm::Value*> values, llvm::IRBuilder<>& builder) {
  // 按 128 位向量合并失败掩码，归约只生成一次标量分支。
  constexpr unsigned kLanes = 2;
  auto* vector_type = llvm::FixedVectorType::get(builder.getDoubleTy(), kLanes);
  auto* zero = llvm::Constant::getNullValue(vector_type);
  auto* mask_type = llvm::FixedVectorType::get(builder.getInt1Ty(), kLanes);
  llvm::Value* failures = llvm::Constant::getNullValue(mask_type);
  for (std::size_t index = 0; index < values.size(); index += kLanes) {
    auto* second = index + 1 < values.size() ? values[index + 1]
                                             : llvm::ConstantFP::get(builder.getDoubleTy(), 1);
    auto* pair = builder.CreateInsertElement(vector_type, values[index], builder.getInt32(0));
    pair = builder.CreateInsertElement(pair, second, builder.getInt32(1));
    // 后续本不会求值的 lane 不能淹没已确定的零。NaN 和原 UNE 一样通过。
    auto* failed = builder.CreateFCmpOEQ(builder.CreateFreeze(pair), zero);
    failures = builder.CreateOr(failures, failed);
  }
  return builder.CreateNot(builder.CreateOrReduce(failures));
}

llvm::BasicBlock* BatchFieldGuards(const FieldLoopPlan& plan,
                                   const FieldGuards& guards,
                                   const llvm::ValueToValueMapTy& mapped,
                                   llvm::BasicBlock& fast_latch) {
  auto* header = plan.loop->getHeader();
  auto* commit =
      fast_latch.splitBasicBlock(fast_latch.getTerminator()->getIterator(), "fields.commit");
  auto* jump = fast_latch.getTerminator();
  llvm::IRBuilder<> builder(jump);
  llvm::SmallVector<llvm::Value*, kInlineFieldLocations> guard_values;
  for (const auto& guard : guards) {
    auto* branch = llvm::cast<llvm::CondBrInst>(mapped.lookup(guard.branch));
    const auto* condition = llvm::cast<llvm::FCmpInst>(branch->getCondition());
    guard_values.push_back(condition->getOperand(0));
    auto* block = branch->getParent();
    branch->getSuccessor(1 - guard.success_edge)->removePredecessor(block);
    llvm::UncondBrInst::Create(branch->getSuccessor(guard.success_edge), branch->getIterator());
    branch->eraseFromParent();
  }
  const auto* first_guard = guards.front().branch->getParent();
  bool speculative = false;
  for (const auto* original : plan.blocks) {
    if (speculative) {
      auto* block = llvm::cast<llvm::BasicBlock>(mapped.lookup(original));
      for (auto& instruction : *block) {
        instruction.dropUBImplyingAttrsAndMetadata();
      }
    }
    speculative |= original == first_guard;
  }
  // 失败边只交付本轮入口值。原循环从未修改的字段重跑本轮并处理首个失败。
  for (auto& phi : header->phis()) {
    phi.addIncoming(mapped.lookup(&phi), &fast_latch);
  }
  auto* ready = BuildFieldGuardCheck(guard_values, builder);
  builder.CreateCondBr(ready, commit, header,
                       llvm::MDBuilder(header->getContext()).createLikelyBranchWeights());
  jump->eraseFromParent();
  return commit;
}

void StageFieldLoop(const FieldLoopPlan& plan) {
  auto& loop = *plan.loop;
  const auto guards = FindFieldGuardBatch(plan);
  auto* function = loop.getHeader()->getParent();
  auto& context = function->getContext();
  llvm::IRBuilder<> guard(loop.getLoopPreheader()->getTerminator());
  llvm::Value* disjoint = guard.getTrue();
  for (const auto& [left, right] : plan.disjoint) {
    disjoint = guard.CreateAnd(
        disjoint, guard.CreateICmpNE(guard.CreateFreeze(left), guard.CreateFreeze(right)));
  }
  llvm::ValueToValueMapTy mapped;
  VersionLoop(loop, *disjoint, mapped, ".fields");
  llvm::MDBuilder metadata(context);
  auto* domain = metadata.createAnonymousAliasScopeDomain("staged fields");
  llvm::SmallVector<llvm::Metadata*, kInlineFieldLocations> scopes;
  const auto field_count = plan.fields.size();
  for (std::size_t index = 0; index < field_count + plan.readers.size(); ++index) {
    scopes.push_back(metadata.createAnonymousAliasScope(domain));
  }
  const auto annotate = [&](llvm::Instruction& instruction, std::size_t index) {
    llvm::SmallVector<llvm::Metadata*, kInlineFieldLocations> separate;
    const auto count = index < field_count ? scopes.size() : field_count;
    for (std::size_t other = 0; other < count; ++other) {
      if (other != index) {
        separate.push_back(scopes[other]);
      }
    }
    AddScope(instruction, llvm::LLVMContext::MD_alias_scope, {scopes[index]});
    AddScope(instruction, llvm::LLVMContext::MD_noalias, separate);
  };
  for (std::size_t index = 0; index < plan.readers.size(); ++index) {
    for (auto* access : plan.readers[index].accesses) {
      annotate(*llvm::cast<llvm::Instruction>(mapped.lookup(access)), field_count + index);
    }
  }
  const auto staged = StageFieldAccesses(plan, mapped, annotate);
  const auto flush_shadow = [&](llvm::IRBuilder<>& builder, std::size_t index) {
    const auto& field = plan.fields[index];
    auto* value = builder.CreateLoad(builder.getDoubleTy(), staged.shadows[index]);
    auto* stored = builder.CreateAlignedStore(value, field.pointer, llvm::Align(alignof(double)));
    stored->copyMetadata(*field.accesses.front());
    annotate(*stored, index);
  };
  auto* fast_latch = llvm::cast<llvm::BasicBlock>(mapped.lookup(loop.getLoopLatch()));
  auto* commit_block =
      guards.empty() ? fast_latch : BatchFieldGuards(plan, guards, mapped, *fast_latch);
  llvm::IRBuilder<> commit(commit_block->getTerminator());
  for (std::size_t index = 0; index < field_count; ++index) {
    flush_shadow(commit, index);
  }
  if (guards.empty()) {
    FlushFieldExitPrefixes(plan, mapped, staged, flush_shadow);
  }
}

class FieldLoopPass : public llvm::PassInfoMixin<FieldLoopPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    const auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    auto& aliases = analyses.getResult<llvm::AAManager>(function);
    llvm::SmallVector<FieldLoopPlan, 2> plans;
    for (auto* loop : loops.getLoopsInPreorder()) {
      if (auto plan = AnalyzeFieldLoop(*loop, dominators, aliases)) {
        plans.push_back(std::move(*plan));
      }
    }
    for (const auto& plan : plans) {
      StageFieldLoop(plan);
    }
    if (plans.empty()) {
      return llvm::PreservedAnalyses::all();
    }
    return llvm::PreservedAnalyses::none();
  }
};

llvm::Instruction* EarlierFieldCalculationAnchor(
    const llvm::Instruction& instruction,
    llvm::ArrayRef<llvm::BasicBlock*> blocks,
    const llvm::DenseMap<llvm::BasicBlock*, std::size_t>& positions,
    const llvm::DominatorTree& dominators,
    std::size_t current) {
  std::size_t first = 0;
  for (const auto& operand : instruction.operands()) {
    const auto* definition = llvm::dyn_cast<llvm::Instruction>(operand.get());
    if (definition != nullptr) {
      if (const auto found = positions.find(definition->getParent()); found != positions.end()) {
        first = std::max(first, found->second);
      }
    }
  }
  if (first >= current) {
    return nullptr;
  }
  auto* anchor = blocks[first]->getTerminator();
  if (std::ranges::any_of(instruction.operands(), [&](const auto& operand) {
        const auto* definition = llvm::dyn_cast<llvm::Instruction>(operand.get());
        return definition != nullptr && !dominators.dominates(definition, anchor);
      })) {
    return nullptr;
  }
  return anchor;
}

std::size_t ScheduleFieldCalculations(llvm::Loop& loop, const llvm::DominatorTree& dominators) {
  const auto blocks = IterationPath(loop);
  llvm::DenseMap<llvm::BasicBlock*, std::size_t> positions;
  for (std::size_t index = 0; index < blocks.size(); ++index) {
    positions.insert({blocks[index], index});
  }
  std::size_t moved = 0;
  for (std::size_t index = 1; index < blocks.size(); ++index) {
    auto* block = blocks[index];
    for (auto cursor = block->begin(); cursor != block->end();) {
      auto& instruction = *cursor++;
      if (!instruction.getType()->isFPOrFPVectorTy() || llvm::isa<llvm::PHINode>(instruction) ||
          instruction.mayReadOrWriteMemory() || instruction.mayHaveSideEffects() ||
          !llvm::isSafeToSpeculativelyExecute(&instruction)) {
        continue;
      }
      auto* anchor =
          EarlierFieldCalculationAnchor(instruction, blocks, positions, dominators, index);
      if (anchor != nullptr) {
        // 仅提前可安全投机的独立计算；依赖次序、守卫和字段提交保持不变。
        instruction.moveBefore(anchor->getIterator());
        ++moved;
      }
    }
  }
  return moved;
}

class FieldCalculationSchedulePass : public llvm::PassInfoMixin<FieldCalculationSchedulePass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    const auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    const auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    std::size_t moved = 0;
    for (auto* loop : loops.getLoopsInPreorder()) {
      if (loop->getSubLoops().empty() && loop->getLoopLatch() != nullptr && CanStageMemory(*loop)) {
        moved += ScheduleFieldCalculations(*loop, dominators);
      }
    }
    return moved == 0 ? llvm::PreservedAnalyses::all() : llvm::PreservedAnalyses::none();
  }
};

class FieldLoopUnrollPass : public llvm::PassInfoMixin<FieldLoopUnrollPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    if (!IsFieldLoopCandidate(function)) {
      return llvm::PreservedAnalyses::all();
    }
    llvm::FunctionPassManager canonicalize;
    canonicalize.addPass(llvm::LoopSimplifyPass());
    canonicalize.addPass(llvm::LCSSAPass());
    llvm::FunctionPassManager unroll;
    unroll.addPass(llvm::LoopUnrollPass(llvm::LoopUnrollOptions(3, true)
                                            .setPartial(false)
                                            .setRuntime(false)
                                            .setPeeling(false)
                                            .setUpperBound(true)));
    unroll.addPass(llvm::InstCombinePass());
    unroll.addPass(llvm::SimplifyCFGPass());
    unroll.addPass(llvm::EarlyCSEPass(true));
    auto preserved = llvm::PreservedAnalyses::all();
    for (unsigned round = 0; round < kMaximumNestedUnrollRounds; ++round) {
      preserved.intersect(canonicalize.run(function, analyses));
      const auto selected = FieldLoopHintsPass::run(function, analyses);
      if (selected.areAllPreserved()) {
        break;
      }
      analyses.invalidate(function, selected);
      preserved.intersect(selected);
      preserved.intersect(unroll.run(function, analyses));
    }
    return preserved;
  }
};

class FieldLoopPipeline : public llvm::PassInfoMixin<FieldLoopPipeline> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    if (!IsFieldLoopCandidate(function)) {
      return llvm::PreservedAnalyses::all();
    }
    llvm::FunctionPassManager preparation;
    preparation.addPass(ScalarFieldAccessPass());
    preparation.addPass(llvm::InstCombinePass());
    preparation.addPass(llvm::EarlyCSEPass(true));
    preparation.addPass(llvm::LoopSimplifyPass());
    preparation.addPass(llvm::LCSSAPass());
    preparation.addPass(FieldEntryPeelPass());
    preparation.addPass(llvm::InstCombinePass());
    preparation.addPass(llvm::SimplifyCFGPass());
    preparation.addPass(llvm::GVNPass(llvm::GVNOptions().setLoadPRE(true).setLoadInLoopPRE(true)));
    preparation.addPass(llvm::LoopSimplifyPass());
    preparation.addPass(llvm::LCSSAPass());
    preparation.addPass(
        llvm::createFunctionToLoopPassAdaptor(llvm::LICMPass(llvm::LICMOptions()), true));
    preparation.addPass(llvm::GVNPass());
    // GVN 可以折叠 LCSSA PHI；克隆前重新封闭所有出口值，才能逐边修复共享出口。
    preparation.addPass(llvm::LoopSimplifyPass());
    preparation.addPass(llvm::LCSSAPass());
    auto preserved = preparation.run(function, analyses);
    llvm::FunctionPassManager transform;
    transform.addPass(FieldLoopPass());
    transform.addPass(llvm::PromotePass());
    transform.addPass(llvm::InstCombinePass());
    transform.addPass(llvm::SimplifyCFGPass());
    transform.addPass(llvm::GVNPass());
    transform.addPass(llvm::DSEPass());
    transform.addPass(llvm::LoopSimplifyPass());
    transform.addPass(llvm::LCSSAPass());
    transform.addPass(
        llvm::createFunctionToLoopPassAdaptor(llvm::LICMPass(llvm::LICMOptions()), true));
    // 先把可安全投机的独立计算归并到同一基本块，再让 SLP 打包平方根与除法。
    transform.addPass(FieldCalculationSchedulePass());
    transform.addPass(llvm::SLPVectorizerPass());
    transform.addPass(llvm::InstCombinePass());
    transform.addPass(llvm::DCEPass());
    preserved.intersect(transform.run(function, analyses));
    return preserved;
  }
};

class LateFieldGuardPass : public llvm::PassInfoMixin<LateFieldGuardPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    if (!IsFieldLoopCandidate(function)) {
      return llvm::PreservedAnalyses::all();
    }
    const auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    llvm::SmallVector<llvm::FCmpInst*, kInlineFieldLocations> guards;
    for (auto& instruction : llvm::instructions(function)) {
      auto* compare = llvm::dyn_cast<llvm::FCmpInst>(&instruction);
      if (compare == nullptr || compare->getPredicate() != llvm::CmpInst::FCMP_UNE ||
          !compare->getOperand(0)->getType()->isDoubleTy() ||
          loops.getLoopFor(compare->getParent()) == nullptr) {
        continue;
      }
      const auto* zero = llvm::dyn_cast<llvm::ConstantFP>(compare->getOperand(1));
      if (zero != nullptr && zero->isZero()) {
        guards.push_back(compare);
      }
    }
    for (auto* guard : guards) {
      auto* operand = guard->getOperand(0);
      if (llvm::cannotBeOrderedLessThanZero(operand,
                                            llvm::SimplifyQuery(function.getDataLayout(), guard))) {
        // 非负值的非零测试可用一次包含 unordered 的大小比较，NaN 仍走成功边。
        guard->setPredicate(llvm::CmpInst::FCMP_UGT);
        continue;
      }
      // 在 SLP 完成后再改写，保留先前平方根与除法的打包机会；两个零仅符号位不同。
      llvm::IRBuilder<> builder(guard);
      auto* bits = builder.CreateBitCast(operand, builder.getInt64Ty());
      auto* shifted = builder.CreateShl(bits, 1);
      guard->replaceAllUsesWith(builder.CreateICmpNE(shifted, builder.getInt64(0)));
      guard->eraseFromParent();
    }
    return guards.empty() ? llvm::PreservedAnalyses::all() : llvm::PreservedAnalyses::none();
  }
};

}  // namespace

void AddFieldLoopUnrollPass(llvm::FunctionPassManager& passes) {
  passes.addPass(FieldLoopUnrollPass());
}

void AddFieldLoopPasses(llvm::FunctionPassManager& passes) {
  passes.addPass(FieldLoopPipeline());
}

void AddLateFieldGuardPasses(llvm::FunctionPassManager& passes) {
  passes.addPass(LateFieldGuardPass());
}

}  // namespace vela::compiler::native
