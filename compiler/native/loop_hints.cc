#include "compiler/native/loop_hints.h"

#include <algorithm>

#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>

namespace vela::compiler::native {
namespace {

constexpr const char* kUnrollPrefix = "llvm.loop.unroll.";
constexpr const char* kUnrollCount = "llvm.loop.unroll.count";

}  // namespace

bool HasLoopUnrollHints(const llvm::Loop& loop) {
  const auto* metadata = loop.getLoopID();
  if (metadata == nullptr) {
    return false;
  }
  return std::ranges::any_of(metadata->operands(), [](const auto& operand) {
    const auto* node = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
    if (node == nullptr || node->getNumOperands() == 0) {
      return false;
    }
    const auto* name = llvm::dyn_cast_or_null<llvm::MDString>(node->getOperand(0));
    return name != nullptr && name->getString().starts_with(kUnrollPrefix);
  });
}

void SetLoopUnrollCount(llvm::Loop& loop, unsigned count, LoopUnrollHints hints) {
  const llvm::StringRef removed = hints == LoopUnrollHints::Replace ? kUnrollPrefix : kUnrollCount;
  loop.addIntLoopAttribute(kUnrollCount, count, {removed});
}

}  // namespace vela::compiler::native
