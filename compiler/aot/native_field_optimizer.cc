#include "compiler/aot/native_field_optimizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/JumpThreading.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/LCSSA.h>
#include <llvm/Transforms/Utils/LoopPeel.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "runtime/native/native_memory_layout.h"
#include "runtime/native/native_symbols.h"
#include "runtime/value/object.h"

namespace vela::compiler::aot {
namespace {

constexpr unsigned kMaximumRepeatedUpdates = 32;
constexpr unsigned kSmallAnalysisSetCapacity = 32;

bool HasStorageTag(const llvm::Instruction& instruction, llvm::StringRef name) {
  const auto* tag = instruction.getMetadata(llvm::LLVMContext::MD_tbaa);
  if (tag == nullptr || tag->getNumOperands() == 0) {
    return false;
  }
  const auto* type = llvm::dyn_cast<llvm::MDNode>(tag->getOperand(0));
  // 物理字段槽继承 payload 域；消费别名事实时沿类型父链识别所属存储区域。
  while (type != nullptr) {
    const auto count = type->getNumOperands();
    if (count == 0) {
      return false;
    }
    const auto* text = llvm::dyn_cast<llvm::MDString>(type->getOperand(0));
    if (text != nullptr && text->getString() == name) {
      return true;
    }
    type = count > 1 ? llvm::dyn_cast<llvm::MDNode>(type->getOperand(1)) : nullptr;
  }
  return false;
}

struct FieldUpdate final {
  llvm::StoreInst* store;
  llvm::BinaryOperator* add;
  llvm::LoadInst* current;
  llvm::LoadInst* delta;
  llvm::LoadInst* object;
  std::int64_t offset;
  std::int64_t delta_offset;
  std::int64_t bytes;

  [[nodiscard]] auto Shape() const {
    return std::tuple(offset, delta_offset, bytes, add->getType());
  }
};

std::optional<FieldUpdate> MatchUpdate(llvm::StoreInst& store, const llvm::Loop& loop) {
  const auto& layout = store.getDataLayout();
  auto* add = llvm::dyn_cast<llvm::BinaryOperator>(store.getValueOperand());
  if (!store.isSimple() || !HasStorageTag(store, "field payloads") || add == nullptr ||
      add->getOpcode() != llvm::Instruction::FAdd || !add->hasOneUse() ||
      add->getFastMathFlags().any()) {
    return std::nullopt;
  }
  std::int64_t offset = 0;
  auto* object = llvm::dyn_cast<llvm::LoadInst>(
      llvm::GetPointerBaseWithConstantOffset(store.getPointerOperand(), offset, layout));
  if (object == nullptr || !loop.contains(object) || !object->isSimple() ||
      !object->getType()->isPointerTy() ||
      object->getMetadata(llvm::LLVMContext::MD_nonnull) == nullptr ||
      !HasStorageTag(*object, "array elements") ||
      std::cmp_less(offset, sizeof(runtime::HeapObject))) {
    return std::nullopt;
  }
  auto* current = llvm::dyn_cast<llvm::LoadInst>(add->getOperand(0));
  auto* delta = llvm::dyn_cast<llvm::LoadInst>(add->getOperand(1));
  if (current == nullptr || delta == nullptr || !current->isSimple() || !delta->isSimple() ||
      !current->hasOneUse() || !delta->hasOneUse() ||
      current->getPointerOperand() != store.getPointerOperand() ||
      !HasStorageTag(*current, "field payloads") || !HasStorageTag(*delta, "field payloads")) {
    return std::nullopt;
  }
  std::int64_t delta_offset = 0;
  if (llvm::GetPointerBaseWithConstantOffset(delta->getPointerOperand(), delta_offset, layout) !=
          object ||
      std::cmp_less(delta_offset, sizeof(runtime::HeapObject))) {
    return std::nullopt;
  }
  return FieldUpdate{
      .store = &store,
      .add = add,
      .current = current,
      .delta = delta,
      .object = object,
      .offset = offset,
      .delta_offset = delta_offset,
      .bytes = static_cast<std::int64_t>(layout.getTypeStoreSize(add->getType()).getFixedValue()),
  };
}

bool HasUniformIndependentFields(std::vector<FieldUpdate>& updates) {
  absl::flat_hash_map<llvm::Value*, std::vector<const FieldUpdate*>> objects;
  for (const auto& update : updates) {
    objects[update.object].push_back(&update);
  }
  const std::vector<const FieldUpdate*>* reference = nullptr;
  for (auto& [object, fields] : objects) {
    std::ranges::sort(fields, {}, [](const auto* field) { return field->Shape(); });
    for (std::size_t index = 0; index < fields.size(); ++index) {
      const auto& field = *fields[index];
      if (index != 0) {
        const auto& previous = *fields[index - 1];
        if (previous.offset + previous.bytes > field.offset) {
          return false;
        }
      }
      for (const auto* written : fields) {
        if (field.delta_offset < written->offset + written->bytes &&
            written->offset < field.delta_offset + field.bytes) {
          return false;
        }
      }
    }
    if (reference != nullptr &&
        !std::ranges::equal(
            *reference, fields, {}, [](const auto* field) { return field->Shape(); },
            [](const auto* field) { return field->Shape(); })) {
      return false;
    }
    reference = &fields;
  }
  return reference != nullptr;
}

std::optional<llvm::BasicBlockEdge> FirstIterationEdge(const llvm::Loop& loop) {
  const auto* branch = llvm::dyn_cast<llvm::BranchInst>(loop.getHeader()->getTerminator());
  if (branch == nullptr || !branch->isConditional()) {
    return std::nullopt;
  }
  const auto* ready = llvm::dyn_cast<llvm::PHINode>(branch->getCondition());
  if (ready == nullptr || ready->getParent() != loop.getHeader() ||
      ready->getNumIncomingValues() != 2) {
    return std::nullopt;
  }
  const auto* repeated =
      llvm::dyn_cast<llvm::ConstantInt>(ready->getIncomingValueForBlock(loop.getLoopLatch()));
  // 入口可以已经 Ready；只要成功回边恒为 true，false 边就最多在首轮执行。
  if (repeated == nullptr || !repeated->isOne()) {
    return std::nullopt;
  }
  return llvm::BasicBlockEdge(loop.getHeader(), branch->getSuccessor(1));
}

bool HasOnlyAdministrativeWrite(const llvm::StoreInst& store,
                                const llvm::Loop& loop,
                                const llvm::DominatorTree& dominators,
                                const std::optional<llvm::BasicBlockEdge>& first) {
  if (!store.isSimple()) {
    return false;
  }
  if (HasStorageTag(store, "shadow roots") && loop.isLoopInvariant(store.getValueOperand())) {
    return true;
  }
  if (!first || !dominators.dominates(*first, store.getParent())) {
    return false;
  }
  std::int64_t offset = 0;
  const auto* base = llvm::GetPointerBaseWithConstantOffset(store.getPointerOperand(), offset,
                                                            store.getDataLayout());
  if (llvm::isa<llvm::AllocaInst>(base)) {
    return true;
  }
  using runtime::NativeMemoryLayout;
  return base == store.getFunction()->getArg(0) &&
         (std::cmp_equal(offset, NativeMemoryLayout::kExecutionFrame) ||
          std::cmp_equal(offset, NativeMemoryLayout::kExecutionDepth));
}

bool CanCompleteBeforeAnyFailure(const llvm::Loop& loop, const std::vector<FieldUpdate>& updates) {
  llvm::SmallVector<llvm::BasicBlock*> exits;
  loop.getExitingBlocks(exits);
  llvm::SmallPtrSet<llvm::BasicBlock*, kSmallAnalysisSetCapacity> visited;
  std::vector<llvm::BasicBlock*> pending;
  for (auto* block : exits) {
    if (block != loop.getLoopLatch()) {
      pending.push_back(block);
    }
  }
  // 失败出口之前不能已经更新前缀，否则合并重复次数会改变错误后可观察的数组内容。
  while (!pending.empty()) {
    auto* block = pending.back();
    pending.pop_back();
    if (!visited.insert(block).second) {
      continue;
    }
    if (std::ranges::any_of(
            updates, [&](const auto& update) { return update.store->getParent() == block; })) {
      return false;
    }
    if (block != loop.getHeader()) {
      for (auto* predecessor : llvm::predecessors(block)) {
        if (loop.contains(predecessor) && predecessor != loop.getLoopLatch()) {
          pending.push_back(predecessor);
        }
      }
    }
  }
  return true;
}

bool CanRepeatInstruction(const llvm::Instruction& instruction,
                          const llvm::DominatorTree& dominators,
                          const std::optional<llvm::BasicBlockEdge>& first) {
  if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    return load->isSimple();
  }
  if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
    if (llvm::isa<llvm::AssumeInst>(call)) {
      return true;
    }
    const auto* callee = call->getCalledFunction();
    return callee != nullptr &&
           callee->getName() == llvm::StringRef(runtime::kNativeInitializeGlobalSymbol) && first &&
           dominators.dominates(*first, call->getParent());
  }
  return !instruction.mayHaveSideEffects();
}

bool HasOnlyUpdateObservations(const llvm::Loop& loop, const std::vector<FieldUpdate>& updates) {
  llvm::SmallPtrSet<const llvm::Value*, kSmallAnalysisSetCapacity> floating;
  for (const auto& update : updates) {
    floating.insert(update.current);
    floating.insert(update.delta);
    floating.insert(update.add);
  }
  for (auto* block : loop.blocks()) {
    for (const auto& instruction : *block) {
      if (instruction.getType()->isFPOrFPVectorTy() && !floating.contains(&instruction)) {
        return false;
      }
      if (llvm::isa<llvm::LoadInst>(instruction) && HasStorageTag(instruction, "field payloads") &&
          !floating.contains(&instruction)) {
        return false;
      }
    }
  }
  return true;
}

std::optional<std::vector<FieldUpdate>> AnalyzeRepeatedFields(
    const llvm::Loop& loop,
    const llvm::DominatorTree& dominators,
    const std::optional<llvm::BasicBlockEdge>& first) {
  std::vector<FieldUpdate> updates;
  for (auto* block : loop.blocks()) {
    for (auto& instruction : *block) {
      auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (store == nullptr) {
        if (!CanRepeatInstruction(instruction, dominators, first)) {
          return std::nullopt;
        }
      } else if (store->getValueOperand()->getType()->isFPOrFPVectorTy()) {
        const auto update = MatchUpdate(*store, loop);
        if (!update) {
          return std::nullopt;
        }
        updates.push_back(*update);
      } else if (!HasOnlyAdministrativeWrite(*store, loop, dominators, first)) {
        return std::nullopt;
      }
    }
  }
  if (updates.empty() || !HasUniformIndependentFields(updates) ||
      !HasOnlyUpdateObservations(loop, updates) || !CanCompleteBeforeAnyFailure(loop, updates)) {
    return std::nullopt;
  }
  return updates;
}

bool IsUnusedCounter(const llvm::PHINode& index,
                     const llvm::Instruction& increment,
                     const llvm::Value& condition) {
  return std::ranges::all_of(index.users(), [&](const auto* user) { return user == &increment; }) &&
         std::ranges::all_of(increment.users(), [&](const auto* user) {
           return user == &index || user == &condition;
         });
}

enum class FieldLoopPhase : std::uint8_t { PeelInitialization, FoldUpdates };

struct RepeatedLoop final {
  llvm::BranchInst* latch;
  unsigned count;
  std::optional<llvm::BasicBlockEdge> first_iteration;
};

std::optional<RepeatedLoop> MatchRepeatedLoop(llvm::Loop& loop,
                                              llvm::ScalarEvolution& evolution,
                                              FieldLoopPhase phase) {
  auto* preheader = loop.getLoopPreheader();
  auto* latch_block = loop.getLoopLatch();
  if (loop.getSubLoops().empty() || preheader == nullptr || latch_block == nullptr) {
    return std::nullopt;
  }
  const auto count = evolution.getSmallConstantTripCount(&loop, latch_block);
  auto* index = phase == FieldLoopPhase::PeelInitialization ? loop.getCanonicalInductionVariable()
                                                            : loop.getInductionVariable(evolution);
  auto* latch = llvm::dyn_cast<llvm::BranchInst>(latch_block->getTerminator());
  if (count < 2 || count > kMaximumRepeatedUpdates || index == nullptr || latch == nullptr ||
      !latch->isConditional()) {
    return std::nullopt;
  }
  auto* increment = llvm::dyn_cast<llvm::Instruction>(index->getIncomingValueForBlock(latch_block));
  if (increment == nullptr || !IsUnusedCounter(*index, *increment, *latch->getCondition())) {
    return std::nullopt;
  }
  // 剥离首轮时允许尚未建立的缓存值；化简 ready、trace 和缓存 PHI 后，
  // 真正折叠的剩余循环只能携带计数器，不能保留用户逐轮状态。
  if (phase == FieldLoopPhase::FoldUpdates &&
      std::ranges::any_of(loop.getHeader()->phis(),
                          [&](const auto& phi) { return &phi != index; })) {
    return std::nullopt;
  }
  return RepeatedLoop{.latch = latch, .count = count, .first_iteration = FirstIterationEdge(loop)};
}

class RepeatedFieldPass final : public llvm::PassInfoMixin<RepeatedFieldPass> {
 public:
  explicit RepeatedFieldPass(FieldLoopPhase phase) : phase_(phase) {}

  llvm::PreservedAnalyses run(llvm::Function& function, llvm::FunctionAnalysisManager& analyses) {
    auto& loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto& evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
    auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    for (auto* loop : loops.getLoopsInPreorder()) {
      const auto matched = MatchRepeatedLoop(*loop, evolution, phase_);
      if (!matched) {
        continue;
      }
      const auto updates = AnalyzeRepeatedFields(*loop, dominators, matched->first_iteration);
      if (!updates) {
        continue;
      }
      if (phase_ == FieldLoopPhase::PeelInitialization) {
        if (matched->first_iteration && llvm::canPeel(loop)) {
          auto& assumptions = analyses.getResult<llvm::AssumptionAnalysis>(function);
          llvm::ValueToValueMapTy values;
          if (llvm::peelLoop(loop, 1, false, &loops, &evolution, dominators, &assumptions, true,
                             values)) {
            return llvm::PreservedAnalyses::none();
          }
        }
        continue;
      }
      for (const auto& update : *updates) {
        llvm::IRBuilder<> builder(update.store);
        llvm::Value* result = update.add;
        for (unsigned iteration = 1; iteration < matched->count; ++iteration) {
          result = builder.CreateFAdd(result, update.delta);
        }
        update.store->setOperand(0, result);
      }
      // 即使数组含重复对象，所有字段也应用同一可交换更新，且增量字段从不写入。
      auto* latch = matched->latch;
      latch->setCondition(llvm::ConstantInt::getBool(function.getContext(),
                                                     !loop->contains(latch->getSuccessor(0))));
      return llvm::PreservedAnalyses::none();
    }
    return llvm::PreservedAnalyses::all();
  }

 private:
  FieldLoopPhase phase_;
};

}  // namespace

void FoldRepeatedFieldUpdates(llvm::Module& module, llvm::TargetMachine& machine) {
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager calls;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder passes(&machine);
  passes.registerModuleAnalyses(modules);
  passes.registerCGSCCAnalyses(calls);
  passes.registerFunctionAnalyses(functions);
  passes.registerLoopAnalyses(loops);
  passes.crossRegisterProxies(loops, functions, calls, modules);
  llvm::FunctionPassManager pipeline;
  pipeline.addPass(llvm::LoopSimplifyPass());
  pipeline.addPass(llvm::LCSSAPass());
  pipeline.addPass(RepeatedFieldPass(FieldLoopPhase::PeelInitialization));
  pipeline.addPass(llvm::InstCombinePass());
  pipeline.addPass(llvm::JumpThreadingPass());
  pipeline.addPass(llvm::SimplifyCFGPass());
  pipeline.addPass(llvm::EarlyCSEPass());
  pipeline.addPass(llvm::DCEPass());
  pipeline.addPass(llvm::LoopSimplifyPass());
  pipeline.addPass(llvm::LCSSAPass());
  pipeline.addPass(RepeatedFieldPass(FieldLoopPhase::FoldUpdates));
  pipeline.addPass(llvm::SimplifyCFGPass());
  pipeline.addPass(llvm::DCEPass());
  for (auto& function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    const auto& loop_info = functions.getResult<llvm::LoopAnalysis>(function);
    auto& evolution = functions.getResult<llvm::ScalarEvolutionAnalysis>(function);
    const auto& dominators = functions.getResult<llvm::DominatorTreeAnalysis>(function);
    const auto candidates = loop_info.getLoopsInPreorder();
    const auto eligible = std::ranges::any_of(candidates, [&](const auto* loop) {
      if (loop->getSubLoops().empty() || loop->getLoopLatch() == nullptr ||
          loop->getLoopPreheader() == nullptr) {
        return false;
      }
      const auto count = evolution.getSmallConstantTripCount(loop, loop->getLoopLatch());
      return count >= 2 && count <= kMaximumRepeatedUpdates &&
             AnalyzeRepeatedFields(*loop, dominators, FirstIterationEdge(*loop)).has_value();
    });
    if (eligible) {
      pipeline.run(function, functions);
    }
  }
}

}  // namespace vela::compiler::aot
