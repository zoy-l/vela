#include "compiler/native/root_store_optimizer.h"

#include <cstdint>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Casting.h>

#include "compiler/native/memory_regions.h"
#include "runtime/native/native_memory_layout.h"
#include "runtime/value/value.h"

namespace vela::compiler::native {
namespace {

using Address = std::pair<const llvm::AllocaInst*, std::int64_t>;
using KnownRoots = absl::flat_hash_map<Address, const llvm::Value*>;
enum class RootStoreChange : std::uint8_t { Unchanged, Changed };
constexpr auto kRecordBytes = static_cast<std::int64_t>(sizeof(runtime::Value));

std::int64_t RecordOffset(std::int64_t offset) {
  return offset - (offset % kRecordBytes);
}

KnownRoots IncomingRoots(const llvm::BasicBlock& block,
                         const absl::flat_hash_map<const llvm::BasicBlock*, KnownRoots>& outgoing) {
  KnownRoots known;
  bool first = true;
  for (const auto* predecessor : llvm::predecessors(&block)) {
    const auto found = outgoing.find(predecessor);
    // 尚未处理的回边没有已证明事实；不凭单次遍历推断循环不变量。
    if (found == outgoing.end()) {
      return {};
    }
    if (first) {
      known = found->second;
      first = false;
      continue;
    }
    for (auto current = known.begin(); current != known.end();) {
      const auto value = found->second.find(current->first);
      if (value == found->second.end() || value->second != current->second) {
        known.erase(current++);
      } else {
        ++current;
      }
    }
  }
  return known;
}

class RootStoreOptimizer final {
 public:
  explicit RootStoreOptimizer(const llvm::Function& function)
      : layout_(&function.getParent()->getDataLayout()) {
    for (const auto& instruction : llvm::instructions(function)) {
      const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (store == nullptr || !HasStorageTag(*store, MemoryRegion::Roots)) {
        continue;
      }
      std::int64_t offset = 0;
      const auto* base =
          llvm::GetPointerBaseWithConstantOffset(store->getPointerOperand(), offset, *layout_);
      if (const auto* allocation = llvm::dyn_cast<llvm::AllocaInst>(base)) {
        root_allocations_.insert(allocation);
        root_records_.emplace(allocation, RecordOffset(offset));
      }
    }
  }

  RootStoreChange Optimize(llvm::Function& function) const {
    absl::flat_hash_map<const llvm::BasicBlock*, KnownRoots> outgoing;
    auto change = RootStoreChange::Unchanged;
    for (auto* block : llvm::ReversePostOrderTraversal<llvm::Function*>(&function)) {
      auto known = IncomingRoots(*block, outgoing);
      for (auto current = block->begin(); current != block->end();) {
        auto& instruction = *current++;
        if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
          if (OptimizeStore(*store, known) == RootStoreChange::Changed) {
            change = RootStoreChange::Changed;
          }
        } else if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
          InvalidateCall(*call, known);
        } else if (instruction.mayWriteToMemory()) {
          known.clear();
        }
      }
      outgoing.emplace(block, std::move(known));
    }
    return change;
  }

 private:
  RootStoreChange OptimizeStore(llvm::StoreInst& store, KnownRoots& known) const {
    if (store.isVolatile() || store.isAtomic()) {
      known.clear();
      return RootStoreChange::Unchanged;
    }
    if (!HasStorageTag(store, MemoryRegion::Roots)) {
      if (store.getMetadata(llvm::LLVMContext::MD_tbaa) == nullptr) {
        known.clear();
      }
      return RootStoreChange::Unchanged;
    }
    std::int64_t offset = 0;
    const auto* base =
        llvm::GetPointerBaseWithConstantOffset(store.getPointerOperand(), offset, *layout_);
    const auto* allocation = llvm::dyn_cast<llvm::AllocaInst>(base);
    const auto* value = store.getValueOperand();
    const auto size = layout_->getTypeStoreSize(value->getType());
    const auto position = offset % kRecordBytes;
    // 只处理物理 Value 的独立 payload/tag 写入；合并写或未知地址撤销全部事实。
    const auto payload = std::cmp_equal(position, runtime::NativeMemoryLayout::kValuePayload) &&
                         size == sizeof(std::uint64_t);
    const auto kind = std::cmp_equal(position, runtime::NativeMemoryLayout::kValueKind) &&
                      size == sizeof(runtime::ValueKind);
    if (allocation == nullptr || (!payload && !kind)) {
      known.clear();
      return RootStoreChange::Unchanged;
    }
    const Address address{allocation, offset};
    const auto found = known.find(address);
    if (found != known.end() && found->second == value) {
      store.eraseFromParent();
      return RootStoreChange::Changed;
    }
    known.insert_or_assign(address, value);
    return RootStoreChange::Unchanged;
  }

  void InvalidateCall(const llvm::CallBase& call, KnownRoots& known) const {
    if (llvm::isa<llvm::MemIntrinsic>(call)) {
      known.clear();
      return;
    }
    // 根地址显式交给调用方时不应用只读借用契约；其他独立的实参/结果存储不影响帧根。
    for (const auto& argument : call.args()) {
      if (!argument->getType()->isPointerTy()) {
        continue;
      }
      std::int64_t offset = 0;
      const auto* base = llvm::GetPointerBaseWithConstantOffset(argument, offset, *layout_);
      const auto* allocation = llvm::dyn_cast<llvm::AllocaInst>(base);
      const auto unknown_root =
          allocation == nullptr && root_allocations_.contains(llvm::dyn_cast<llvm::AllocaInst>(
                                       llvm::getUnderlyingObject(argument)));
      if (unknown_root ||
          (allocation != nullptr && root_records_.contains({allocation, RecordOffset(offset)}))) {
        known.clear();
        return;
      }
    }
  }

  const llvm::DataLayout* layout_;
  absl::flat_hash_set<Address> root_records_;
  absl::flat_hash_set<const llvm::AllocaInst*> root_allocations_;
};

class RootStorePass final : public llvm::PassInfoMixin<RootStorePass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& /*analyses*/) {
    if (!function.hasFnAttribute(kNativeReadOnlyRootsAttribute) ||
        RootStoreOptimizer(function).Optimize(function) == RootStoreChange::Unchanged) {
      return llvm::PreservedAnalyses::all();
    }
    llvm::PreservedAnalyses preserved;
    preserved.preserveSet<llvm::CFGAnalyses>();
    return preserved;
  }
};

}  // namespace

void AddRootStorePasses(llvm::FunctionPassManager& passes) {
  passes.addPass(RootStorePass());
}

}  // namespace vela::compiler::native
