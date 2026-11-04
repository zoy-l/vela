#include "compiler/native/integer_bit_optimizer.h"

#include <cstdint>
#include <limits>
#include <optional>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/KnownBits.h>

namespace vela::compiler::native {
namespace {

struct MaskedShift final {
  llvm::Value* input;
  const llvm::ConstantInt* amount;
  const llvm::ConstantInt* mask;
};

std::optional<MaskedShift> MatchShift(llvm::Value* value, unsigned opcode) {
  using namespace llvm::PatternMatch;
  llvm::ConstantInt* mask = nullptr;
  llvm::Value* unmasked = nullptr;
  if (match(value, m_c_And(m_Value(unmasked), m_ConstantInt(mask)))) {
    value = unmasked;
  }
  const auto* shift = llvm::dyn_cast<llvm::BinaryOperator>(value);
  if (shift == nullptr || shift->getOpcode() != opcode) {
    return std::nullopt;
  }
  const auto* amount = llvm::dyn_cast<llvm::ConstantInt>(shift->getOperand(1));
  if (amount == nullptr) {
    return std::nullopt;
  }
  return MaskedShift{.input = shift->getOperand(0), .amount = amount, .mask = mask};
}

struct WordRotation final {
  llvm::Value* input;
  unsigned width;
  unsigned right;
};

std::optional<WordRotation> MatchRotation(llvm::Instruction& instruction) {
  using namespace llvm::PatternMatch;
  if (!instruction.getType()->isIntegerTy()) {
    return std::nullopt;
  }
  llvm::Value* combined = &instruction;
  llvm::ConstantInt* mask = nullptr;
  llvm::Value* unmasked = nullptr;
  const auto masked = match(combined, m_c_And(m_Value(unmasked), m_ConstantInt(mask)));
  if (masked) {
    combined = unmasked;
  }
  const auto* operation = llvm::dyn_cast<llvm::BinaryOperator>(combined);
  if (operation == nullptr || operation->getOpcode() != llvm::Instruction::Or) {
    return std::nullopt;
  }
  auto left = MatchShift(operation->getOperand(0), llvm::Instruction::Shl);
  auto right = MatchShift(operation->getOperand(1), llvm::Instruction::LShr);
  if (!left || !right) {
    left = MatchShift(operation->getOperand(1), llvm::Instruction::Shl);
    right = MatchShift(operation->getOperand(0), llvm::Instruction::LShr);
  }
  if (!left || !right || left->input != right->input || (!masked && left->mask == nullptr)) {
    return std::nullopt;
  }
  const auto& width_mask = masked ? mask->getValue() : left->mask->getValue();
  const auto width = width_mask.getActiveBits();
  if (width != std::numeric_limits<std::uint8_t>::digits &&
      width != std::numeric_limits<std::uint16_t>::digits &&
      width != std::numeric_limits<std::uint32_t>::digits) {
    return std::nullopt;
  }
  const auto source_width = instruction.getType()->getIntegerBitWidth();
  const auto left_amount = left->amount->getLimitedValue(source_width);
  const auto right_amount = right->amount->getLimitedValue(source_width);
  if (width == 0 || width >= source_width || left_amount == 0 || right_amount == 0 ||
      left_amount + right_amount != width) {
    return std::nullopt;
  }
  const auto low_bits = llvm::APInt::getLowBitsSet(source_width, width);
  if (masked && width_mask != low_bits) {
    return std::nullopt;
  }
  const auto left_bits = low_bits.shl(static_cast<unsigned>(left_amount)) & low_bits;
  const auto right_bits = low_bits.lshr(static_cast<unsigned>(right_amount));
  if ((left->mask != nullptr && (left->mask->getValue() & left_bits) != left_bits) ||
      (right->mask != nullptr && (right->mask->getValue() & right_bits) != right_bits) ||
      (!masked && left->mask->getValue() != left_bits)) {
    return std::nullopt;
  }
  return WordRotation{
      .input = left->input,
      .width = width,
      .right = static_cast<unsigned>(right_amount),
  };
}

class IntegerBitPass : public llvm::PassInfoMixin<IntegerBitPass> {
 public:
  static llvm::PreservedAnalyses run(llvm::Function& function,
                                     llvm::FunctionAnalysisManager& analyses) {
    auto& assumptions = analyses.getResult<llvm::AssumptionAnalysis>(function);
    auto& dominators = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    const auto& layout = function.getDataLayout();
    bool changed = false;
    for (auto& instruction : llvm::make_early_inc_range(llvm::instructions(function))) {
      const auto rotation = MatchRotation(instruction);
      if (!rotation) {
        continue;
      }
      const auto known =
          llvm::computeKnownBits(rotation->input, layout, &assumptions, &instruction, &dominators);
      // 高位参与逻辑右移，最终掩码不会抹去其影响；只有证明高位为零才能收窄为旋转。
      if (known.Zero.countLeadingOnes() <
          instruction.getType()->getIntegerBitWidth() - rotation->width) {
        continue;
      }
      llvm::IRBuilder<> builder(&instruction);
      auto* word = builder.getIntNTy(rotation->width);
      auto* input = builder.CreateTrunc(rotation->input, word);
      auto* result =
          builder.CreateIntrinsic(llvm::Intrinsic::fshr, {word},
                                  {input, input, llvm::ConstantInt::get(word, rotation->right)});
      instruction.replaceAllUsesWith(builder.CreateZExt(result, instruction.getType()));
      instruction.eraseFromParent();
      changed = true;
    }
    return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
  }
};

}  // namespace

void AddIntegerBitPasses(llvm::FunctionPassManager& passes) {
  passes.addPass(IntegerBitPass());
}

}  // namespace vela::compiler::native
