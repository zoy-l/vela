#include "compiler/native/field_loop_optimizer.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
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
#include <llvm/Transforms/Utils/LoopUtils.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <llvm/Transforms/Vectorize/SLPVectorizer.h>

#include "compiler/native/array_reduction_optimizer.h"
#include "compiler/native/loop_cloning.h"
#include "compiler/native/memory_regions.h"
#include "runtime/native/native_abi.h"

namespace vela::compiler::native {
namespace {

constexpr unsigned kMaximumFieldLoopTrips = 8;
constexpr unsigned kMaximumNestedUnrollRounds = 16;
constexpr std::size_t kMaximumFieldLoopInstructions = 4096;
constexpr std::size_t kMaximumFieldLocations = 64;

bool HasFloatingFields(const llvm::Function& function) {
  return std::ranges::any_of(llvm::instructions(function), [](const auto& instruction) {
    const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
    return store != nullptr && store->getValueOperand()->getType()->isFPOrFPVectorTy() &&
           HasStorageTag(*store, MemoryRegion::FieldPayloads);
  });
}

class FieldLoopHintsPass : public llvm::PassInfoMixin<FieldLoopHintsPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    const auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
    for (auto* loop : loops.getLoopsInPreorder()) {
      const auto trips = evolution.getSmallConstantMaxTripCount(loop);
      if (trips == 0 || trips > kMaximumFieldLoopTrips)
        continue;
      std::size_t instructions = 0;
      bool fields = false;
      bool calls = false;
      for (auto* block : loop->blocks()) {
        instructions += block->size();
        for (auto& instruction : *block) {
          if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
            fields |= store->getValueOperand()->getType()->isFPOrFPVectorTy() &&
                      HasStorageTag(*store, MemoryRegion::FieldPayloads);
          }
          calls |= llvm::isa<llvm::CallBase>(instruction) &&
                   !llvm::isa<llvm::IntrinsicInst>(instruction);
        }
      }
      if (!fields || calls || instructions * trips > kMaximumFieldLoopInstructions ||
          function.getInstructionCount() + instructions * (trips - 1) >
              4 * kMaximumFieldLoopInstructions)
        continue;
      if (trips == 1) {
        auto* latch = loop->getLoopLatch();
        auto* branch =
            latch == nullptr ? nullptr : llvm::dyn_cast<llvm::CondBrInst>(latch->getTerminator());
        if (branch == nullptr ||
            (loop->contains(branch->getSuccessor(0)) == loop->contains(branch->getSuccessor(1))))
          continue;
        branch->setCondition(llvm::ConstantInt::getBool(function.getContext(),
                                                        !loop->contains(branch->getSuccessor(0))));
        return llvm::PreservedAnalyses::none();
      }
      if (auto* metadata = loop->getLoopID()) {
        loop->setLoopID(*llvm::makeFollowupLoopID(metadata, {}, "llvm.loop.unroll.", true));
      }
      llvm::addStringMetadataToLoop(loop, "llvm.loop.unroll.count", trips);
      return llvm::PreservedAnalyses::none();
    }
    return llvm::PreservedAnalyses::all();
  }
};

struct FieldLocation final {
  llvm::Value* pointer;
  llvm::SmallVector<llvm::Instruction*, 8> accesses;
  llvm::Instruction* first = nullptr;
};

class ScalarFieldAccessPass : public llvm::PassInfoMixin<ScalarFieldAccessPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& /*analyses*/) {
    llvm::SmallVector<llvm::Instruction*, 16> accesses;
    for (auto& instruction : llvm::instructions(function)) {
      auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      auto* type = load != nullptr    ? load->getType()
                   : store != nullptr ? store->getValueOperand()->getType()
                                      : nullptr;
      const auto* vector = llvm::dyn_cast_or_null<llvm::FixedVectorType>(type);
      if (vector != nullptr && vector->getElementType()->isDoubleTy() &&
          vector->getNumElements() <= 8 &&
          HasStorageTag(instruction, MemoryRegion::FieldPayloads) &&
          ((load != nullptr && load->isSimple()) || (store != nullptr && store->isSimple()))) {
        accesses.push_back(&instruction);
      }
    }
    for (auto* access : accesses) {
      auto* load = llvm::dyn_cast<llvm::LoadInst>(access);
      auto* store = llvm::dyn_cast<llvm::StoreInst>(access);
      auto* type = llvm::cast<llvm::FixedVectorType>(
          load != nullptr ? load->getType() : store->getValueOperand()->getType());
      auto* pointer = load != nullptr ? load->getPointerOperand() : store->getPointerOperand();
      const auto alignment = load != nullptr ? load->getAlign() : store->getAlign();
      llvm::IRBuilder<> builder(access);
      llvm::Value* value =
          load != nullptr ? llvm::PoisonValue::get(type) : store->getValueOperand();
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
        scalar->copyMetadata(*access);
      }
      if (load != nullptr)
        load->replaceAllUsesWith(value);
      access->eraseFromParent();
    }
    return accesses.empty() ? llvm::PreservedAnalyses::all() : llvm::PreservedAnalyses::none();
  }
};

struct FieldLoopPlan final {
  llvm::Loop* loop;
  llvm::SmallVector<llvm::BasicBlock*, 16> blocks;
  llvm::SmallVector<FieldLocation, 16> fields;
  llvm::SmallVector<FieldLocation, 8> readers;
  llvm::SmallVector<std::pair<llvm::Value*, llvm::Value*>, 16> disjoint;
};

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
  const auto* constant = llvm::dyn_cast<llvm::Constant>(store.getValueOperand());
  if (constant == nullptr)
    return false;
  if (constant->isNullValue() && HasStorageTag(store, MemoryRegion::Roots))
    return true;
  const auto* kind = llvm::dyn_cast<llvm::ConstantInt>(constant);
  return kind != nullptr &&
         kind->equalsInt(static_cast<unsigned>(runtime::NativeValueKind::Double)) &&
         HasStorageTag(store, MemoryRegion::FieldKinds);
}

bool CanStageMemory(const llvm::Loop& loop) {
  for (auto* block : loop.blocks()) {
    const auto* terminator = block->getTerminator();
    if (!llvm::isa<llvm::UncondBrInst>(terminator) && !llvm::isa<llvm::CondBrInst>(terminator))
      return false;
    for (const auto& instruction : *block) {
      if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (!load->isSimple())
          return false;
      } else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        if (!store->isSimple())
          return false;
      } else if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        if (llvm::isa<llvm::AssumeInst>(call))
          continue;
        if (!llvm::isa<llvm::IntrinsicInst>(call) || !call->doesNotAccessMemory() ||
            call->mayThrow() || !call->willReturn() || call->isConvergent())
          return false;
      } else if (instruction.mayReadOrWriteMemory() || instruction.mayHaveSideEffects()) {
        return false;
      }
    }
  }
  return true;
}

bool HasStablePointer(const llvm::Value* pointer,
                      const llvm::Loop& loop,
                      llvm::AAResults& aliases) {
  if (loop.isLoopInvariant(pointer))
    return true;
  if (auto* address = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer)) {
    return HasStablePointer(address->getPointerOperand(), loop, aliases) &&
           std::ranges::all_of(address->indices(), [&](const auto& index) {
             return loop.isLoopInvariant(index.get());
           });
  }
  const auto* load = llvm::dyn_cast<llvm::LoadInst>(pointer);
  if (load == nullptr || !load->isSimple() || !load->getType()->isPointerTy() ||
      !loop.isLoopInvariant(load->getPointerOperand()) ||
      !HasStorageTag(*load, MemoryRegion::Elements))
    return false;
  const auto location = llvm::MemoryLocation::get(load);
  for (auto* block : loop.blocks()) {
    for (const auto& instruction : *block) {
      const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (store != nullptr && !aliases.isNoAlias(location, llvm::MemoryLocation::get(store))) {
        return false;
      }
    }
  }
  return true;
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
      if (!loop->getSubLoops().empty() || !CanStageMemory(*loop))
        continue;
      if (!llvm::canPeel(loop))
        continue;
      std::size_t instructions = 0;
      bool needs_entry = false;
      bool eligible = true;
      for (auto* block : loop->blocks()) {
        instructions += block->size();
        for (auto& instruction : *block) {
          const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
          if (store == nullptr || IsAdministrative(*store))
            continue;
          auto* pointer = store->getPointerOperand();
          eligible &= store->getValueOperand()->getType()->isDoubleTy() &&
                      HasStorageTag(*store, MemoryRegion::FieldPayloads) &&
                      HasStablePointer(pointer, *loop, aliases);
          needs_entry |= !IsEntryInvariant(pointer, *loop, dominators);
        }
      }
      if (!eligible || !needs_entry || instructions > kMaximumFieldLoopInstructions)
        continue;
      auto& assumptions = analyses.getResult<llvm::AssumptionAnalysis>(function);
      llvm::SmallVector<llvm::LoadInst*, 8> stable;
      for (auto* block : loop->blocks()) {
        for (auto& instruction : *block) {
          auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
          if (load != nullptr && load->getType()->isPointerTy() &&
              HasStablePointer(load, *loop, aliases))
            stable.push_back(load);
        }
      }
      llvm::ValueToValueMapTy mapped;
      // 首轮仍在原守卫后读取；完整成功后，剩余循环才能复用已经可用的固定数组元素。
      llvm::peelLoop(loop, 1, false, &loops, &evolution, dominators, &assumptions, true, mapped);
      const auto* entry = loop->getLoopPreheader()->getTerminator();
      for (auto* load : stable) {
        llvm::Value* value = mapped.lookup(load);
        const auto* definition = llvm::dyn_cast_or_null<llvm::Instruction>(value);
        if (value != nullptr &&
            (definition == nullptr || dominators.dominates(definition, entry))) {
          // 别名分析已证明循环不写该元素；借用成功首轮的真实读取，无需投机读取或额外表。
          load->replaceAllUsesWith(value);
          load->eraseFromParent();
        }
      }
      return llvm::PreservedAnalyses::none();
    }
    return llvm::PreservedAnalyses::all();
  }
};

llvm::SmallVector<llvm::BasicBlock*, 16> IterationPath(llvm::Loop& loop) {
  auto* latch = loop.getLoopLatch();
  llvm::SmallVector<llvm::BasicBlock*, 16> blocks;
  auto* current = loop.getHeader();
  while (true) {
    if (blocks.size() == loop.getBlocks().size())
      return {};
    blocks.push_back(current);
    if (current == latch)
      break;
    llvm::BasicBlock* next = nullptr;
    for (auto* successor : llvm::successors(current)) {
      if (loop.contains(successor)) {
        if (next != nullptr || successor == loop.getHeader()) {
          return {};
        }
        next = successor;
      }
    }
    if (next == nullptr)
      return {};
    current = next;
  }
  if (blocks.size() != loop.getBlocks().size())
    return {};
  return blocks;
}

std::optional<FieldLoopPlan> AnalyzeFieldLoop(llvm::Loop& loop,
                                              llvm::DominatorTree& dominators,
                                              llvm::AAResults& aliases) {
  auto* preheader = loop.getLoopPreheader();
  auto* latch = loop.getLoopLatch();
  if (preheader == nullptr || latch == nullptr || !loop.getSubLoops().empty() ||
      !loop.isLoopExiting(latch) || !CanStageMemory(loop))
    return std::nullopt;
  auto blocks = IterationPath(loop);
  if (blocks.empty())
    return std::nullopt;
  FieldLoopPlan plan{
      .loop = &loop, .blocks = std::move(blocks), .fields = {}, .readers = {}, .disjoint = {}};
  llvm::DenseMap<llvm::Value*, std::size_t> fields;
  std::size_t instructions = 0;
  std::size_t writes = 0;
  for (auto* block : loop.blocks()) {
    instructions += block->size();
    if (instructions > kMaximumFieldLoopInstructions)
      return std::nullopt;
    for (auto& instruction : *block) {
      auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (store == nullptr)
        continue;
      auto* pointer = store->getPointerOperand();
      if (IsAdministrative(*store)) {
        continue;
      } else {
        if (!IsEntryInvariant(pointer, loop, dominators) ||
            !dominators.dominates(store, latch->getTerminator()) ||
            !store->getValueOperand()->getType()->isDoubleTy() || store->getAlign().value() < 8 ||
            !HasStorageTag(*store, MemoryRegion::FieldPayloads)) {
          return std::nullopt;
        }
        auto [position, inserted] = fields.try_emplace(pointer, plan.fields.size());
        if (inserted)
          plan.fields.push_back({.pointer = pointer, .accesses = {}});
        plan.fields[position->second].accesses.push_back(store);
        ++writes;
      }
    }
  }
  if (plan.fields.empty() || plan.fields.size() >= writes ||
      plan.fields.size() > kMaximumFieldLocations)
    return std::nullopt;
  llvm::DenseMap<llvm::Value*, std::size_t> readers;
  for (auto* block : loop.blocks()) {
    for (auto& instruction : *block) {
      auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr)
        continue;
      const auto location = llvm::MemoryLocation::get(load);
      auto* pointer = load->getPointerOperand();
      if (const auto found = fields.find(pointer); found != fields.end()) {
        if (!load->getType()->isDoubleTy())
          return std::nullopt;
        plan.fields[found->second].accesses.push_back(load);
        continue;
      }
      const auto overlaps = std::ranges::any_of(plan.fields, [&](const auto& field) {
        return !aliases.isNoAlias(location, Location(field.accesses.front()));
      });
      if (!overlaps)
        continue;
      if (!load->getType()->isDoubleTy() || load->getAlign().value() < 8 ||
          !IsEntryInvariant(pointer, loop, dominators))
        return std::nullopt;
      auto [position, inserted] = readers.try_emplace(pointer, plan.readers.size());
      if (inserted)
        plan.readers.push_back({.pointer = pointer, .accesses = {}});
      plan.readers[position->second].accesses.push_back(load);
    }
  }
  if (plan.fields.size() + plan.readers.size() > kMaximumFieldLocations)
    return std::nullopt;
  for (auto& field : plan.fields) {
    for (auto* first : field.accesses) {
      if (dominators.dominates(first, latch->getTerminator()) &&
          std::ranges::all_of(field.accesses, [&](auto* access) {
            return first == access || dominators.dominates(first, access);
          })) {
        field.first = first;
        break;
      }
    }
    if (field.first == nullptr) {
      return std::nullopt;
    }
  }
  for (std::size_t left = 0; left < plan.fields.size(); ++left) {
    const auto& field = plan.fields[left];
    const auto append = [&](const FieldLocation& right) {
      if (!aliases.isNoAlias(Location(field.accesses.front()), Location(right.accesses.front()))) {
        plan.disjoint.emplace_back(field.pointer, right.pointer);
      }
    };
    for (std::size_t right = left + 1; right < plan.fields.size(); ++right)
      append(plan.fields[right]);
    for (const auto& reader : plan.readers)
      append(reader);
  }
  std::size_t exits = 0;
  for (auto* block : plan.blocks) {
    if (block == loop.getLoopLatch())
      continue;
    for (auto* successor : llvm::successors(block))
      exits += !loop.contains(successor);
  }
  // 预算覆盖首轮剥离、克隆、别名守卫及所有失败出口的最坏前缀提交。
  if (2 * instructions + 4 * plan.disjoint.size() + 2 * plan.fields.size() * (exits + 2) + exits >
      kMaximumFieldLoopInstructions) {
    return std::nullopt;
  }
  return plan;
}

void AddScope(llvm::Instruction& instruction,
              unsigned kind,
              llvm::ArrayRef<llvm::Metadata*> values) {
  llvm::SmallVector<llvm::Metadata*, 16> combined;
  if (auto* existing = instruction.getMetadata(kind)) {
    for (const auto& operand : existing->operands())
      combined.push_back(operand.get());
  }
  combined.append(values.begin(), values.end());
  instruction.setMetadata(kind, llvm::MDNode::get(instruction.getContext(), combined));
}

void StageFieldLoop(const FieldLoopPlan& plan) {
  auto& loop = *plan.loop;
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
  llvm::SmallVector<llvm::Metadata*, 16> scopes;
  for (std::size_t index = 0; index < plan.fields.size() + plan.readers.size(); ++index) {
    scopes.push_back(metadata.createAnonymousAliasScope(domain));
  }
  const auto annotate = [&](llvm::Instruction& instruction, std::size_t index) {
    llvm::SmallVector<llvm::Metadata*, 16> separate;
    const auto count = index < plan.fields.size() ? scopes.size() : plan.fields.size();
    for (std::size_t other = 0; other < count; ++other) {
      if (other != index)
        separate.push_back(scopes[other]);
    }
    AddScope(instruction, llvm::LLVMContext::MD_alias_scope, {scopes[index]});
    AddScope(instruction, llvm::LLVMContext::MD_noalias, separate);
  };
  for (std::size_t index = 0; index < plan.readers.size(); ++index) {
    for (auto* access : plan.readers[index].accesses) {
      annotate(*llvm::cast<llvm::Instruction>(mapped.lookup(access)), plan.fields.size() + index);
    }
  }
  auto* fast_latch = llvm::cast<llvm::BasicBlock>(mapped.lookup(loop.getLoopLatch()));
  llvm::IRBuilder<> commit(fast_latch->getTerminator());
  llvm::IRBuilder<> allocation(&function->getEntryBlock(), function->getEntryBlock().begin());
  llvm::SmallVector<llvm::AllocaInst*, 16> shadows;
  llvm::DenseMap<llvm::Instruction*, std::size_t> written_fields;
  for (std::size_t index = 0; index < plan.fields.size(); ++index) {
    const auto& field = plan.fields[index];
    auto* shadow = allocation.CreateAlloca(allocation.getDoubleTy(), nullptr, "field.shadow");
    shadows.push_back(shadow);
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
        }
      } else {
        llvm::cast<llvm::StoreInst>(cloned)->setOperand(1, shadow);
        written_fields.insert({access, index});
      }
    }
    auto* value = commit.CreateLoad(commit.getDoubleTy(), shadow);
    auto* stored = commit.CreateAlignedStore(value, field.pointer, llvm::Align(8));
    stored->copyMetadata(*field.accesses.front());
    annotate(*stored, index);
  }
  llvm::SmallVector<bool, 16> written(plan.fields.size(), false);
  for (auto* block : plan.blocks) {
    for (auto& instruction : *block) {
      if (const auto found = written_fields.find(&instruction); found != written_fields.end()) {
        written[found->second] = true;
      }
    }
    if (block == loop.getLoopLatch() ||
        std::ranges::none_of(written, [](auto value) { return value; }))
      continue;
    auto* cloned = llvm::cast<llvm::BasicBlock>(mapped.lookup(block));
    auto* branch = cloned->getTerminator();
    for (unsigned edge = 0; edge < branch->getNumSuccessors(); ++edge) {
      auto* successor = block->getTerminator()->getSuccessor(edge);
      if (loop.contains(successor))
        continue;
      auto* flush = llvm::BasicBlock::Create(context, "fields.prefix", function, successor);
      llvm::IRBuilder<> prefix(flush);
      // 唯一正常路径明确每个失败边之前已执行的写入；只提交该前缀，不重放计算。
      for (std::size_t index = 0; index < plan.fields.size(); ++index) {
        if (!written[index])
          continue;
        const auto& field = plan.fields[index];
        auto* value = prefix.CreateLoad(prefix.getDoubleTy(), shadows[index]);
        auto* stored = prefix.CreateAlignedStore(value, field.pointer, llvm::Align(8));
        stored->copyMetadata(*field.accesses.front());
        annotate(*stored, index);
      }
      prefix.CreateBr(successor);
      for (auto& phi : successor->phis()) {
        phi.setIncomingBlock(static_cast<unsigned>(phi.getBasicBlockIndex(cloned)), flush);
      }
      branch->setSuccessor(edge, flush);
    }
  }
  // 标签和清根保持原位置；所有可观察出口均已提交当前写入前缀。
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
    for (const auto& plan : plans)
      StageFieldLoop(plan);
    if (plans.empty())
      return llvm::PreservedAnalyses::all();
    return llvm::PreservedAnalyses::none();
  }
};

class FieldCalculationSchedulePass : public llvm::PassInfoMixin<FieldCalculationSchedulePass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    const auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    const auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    bool changed = false;
    for (auto* loop : loops.getLoopsInPreorder()) {
      if (!loop->getSubLoops().empty() || loop->getLoopLatch() == nullptr || !CanStageMemory(*loop))
        continue;
      const auto blocks = IterationPath(*loop);
      llvm::DenseMap<llvm::BasicBlock*, std::size_t> positions;
      for (std::size_t index = 0; index < blocks.size(); ++index)
        positions.insert({blocks[index], index});
      for (std::size_t index = 1; index < blocks.size(); ++index) {
        auto* block = blocks[index];
        for (auto cursor = block->begin(); cursor != block->end();) {
          auto& instruction = *cursor++;
          if (!instruction.getType()->isFPOrFPVectorTy() || llvm::isa<llvm::PHINode>(instruction) ||
              instruction.mayReadOrWriteMemory() || instruction.mayHaveSideEffects() ||
              !llvm::isSafeToSpeculativelyExecute(&instruction))
            continue;
          std::size_t first = 0;
          for (const auto& operand : instruction.operands()) {
            const auto* definition = llvm::dyn_cast<llvm::Instruction>(operand.get());
            if (definition != nullptr) {
              if (const auto found = positions.find(definition->getParent());
                  found != positions.end())
                first = std::max(first, found->second);
            }
          }
          if (first >= index)
            continue;
          auto* anchor = blocks[first]->getTerminator();
          if (std::ranges::any_of(instruction.operands(), [&](const auto& operand) {
                const auto* definition = llvm::dyn_cast<llvm::Instruction>(operand.get());
                return definition != nullptr && !dominators.dominates(definition, anchor);
              }))
            continue;
          // 仅提前无内存效果且可安全投机的计算；依赖次序、守卫和所有字段提交保持不变。
          instruction.moveBefore(anchor->getIterator());
          changed = true;
        }
      }
    }
    return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
  }
};

class FieldLoopPipeline : public llvm::PassInfoMixin<FieldLoopPipeline> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    if (!function.hasFnAttribute(kNativeArrayReductionCandidateAttribute) ||
        !HasFloatingFields(function))
      return llvm::PreservedAnalyses::all();
    auto preserved = llvm::PreservedAnalyses::all();
    for (unsigned round = 0; round < kMaximumNestedUnrollRounds; ++round) {
      llvm::FunctionPassManager hints;
      hints.addPass(llvm::LoopSimplifyPass());
      hints.addPass(llvm::LCSSAPass());
      hints.addPass(FieldLoopHintsPass());
      const auto selected = hints.run(function, analyses);
      preserved.intersect(selected);
      if (selected.areAllPreserved())
        break;
      llvm::FunctionPassManager preparation;
      preparation.addPass(
          llvm::LoopUnrollPass(llvm::LoopUnrollOptions(3, true).setUpperBound(true)));
      preparation.addPass(llvm::InstCombinePass());
      preparation.addPass(llvm::SimplifyCFGPass());
      preparation.addPass(llvm::EarlyCSEPass(true));
      preserved.intersect(preparation.run(function, analyses));
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
    preserved.intersect(preparation.run(function, analyses));
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
    transform.addPass(llvm::SLPVectorizerPass());
    transform.addPass(llvm::InstCombinePass());
    transform.addPass(FieldCalculationSchedulePass());
    transform.addPass(llvm::DCEPass());
    preserved.intersect(transform.run(function, analyses));
    return preserved;
  }
};

class LateFieldGuardPass : public llvm::PassInfoMixin<LateFieldGuardPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    if (!function.hasFnAttribute(kNativeArrayReductionCandidateAttribute) ||
        !HasFloatingFields(function))
      return llvm::PreservedAnalyses::all();
    const auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    llvm::SmallVector<llvm::FCmpInst*, 16> guards;
    for (auto& instruction : llvm::instructions(function)) {
      auto* compare = llvm::dyn_cast<llvm::FCmpInst>(&instruction);
      if (compare == nullptr || compare->getPredicate() != llvm::CmpInst::FCMP_UNE ||
          !compare->getOperand(0)->getType()->isDoubleTy() ||
          loops.getLoopFor(compare->getParent()) == nullptr)
        continue;
      const auto* zero = llvm::dyn_cast<llvm::ConstantFP>(compare->getOperand(1));
      if (zero != nullptr && zero->isZero())
        guards.push_back(compare);
    }
    for (auto* guard : guards) {
      llvm::IRBuilder<> builder(guard);
      // 在 SLP 完成后再改写，保留先前平方根与除法的打包机会；两个零仅符号位不同。
      auto* bits = builder.CreateBitCast(guard->getOperand(0), builder.getInt64Ty());
      auto* shifted = builder.CreateShl(bits, 1);
      guard->replaceAllUsesWith(builder.CreateICmpNE(shifted, builder.getInt64(0)));
      guard->eraseFromParent();
    }
    return guards.empty() ? llvm::PreservedAnalyses::all() : llvm::PreservedAnalyses::none();
  }
};

}  // namespace

void AddFieldLoopPasses(llvm::FunctionPassManager& passes) {
  passes.addPass(FieldLoopPipeline());
}

void AddLateFieldGuardPasses(llvm::FunctionPassManager& passes) {
  passes.addPass(LateFieldGuardPass());
}

}  // namespace vela::compiler::native
