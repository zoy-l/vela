#include "base/integer_arithmetic.h"

#include <cstdint>
#include <limits>

#include "gtest/gtest.h"

#include "base/execution_status.h"
#include "base/opcode.h"

namespace vela {
namespace {

TEST(IntegerArithmeticTest, ModuloPreservesDivisionErrorsAtSignedBoundaries) {
  constexpr auto kMinimum = std::numeric_limits<std::int64_t>::min();
  const auto overflow = EvaluateIntegerArithmetic(Opcode::ModInt, kMinimum, -1);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), ExecutionStatus::IntegerOverflow);
  const auto zero = EvaluateIntegerArithmetic(Opcode::ModInt, kMinimum, 0);
  ASSERT_FALSE(zero);
  EXPECT_EQ(zero.error(), ExecutionStatus::DivisionByZero);
  EXPECT_EQ(EvaluateIntegerArithmetic(Opcode::ModInt, kMinimum, 1), 0);
  EXPECT_EQ(EvaluateIntegerArithmetic(Opcode::ModInt, -43, 2), -1);
}

TEST(IntegerArithmeticTest, ShiftsMaskTheCountBeforePreservingSignedBits) {
  constexpr auto kWidth = std::numeric_limits<std::uint64_t>::digits;
  constexpr auto kMinimum = std::numeric_limits<std::int64_t>::min();
  constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(EvaluateIntegerArithmetic(Opcode::ShiftLeftInt, 1, kWidth), 1);
  EXPECT_EQ(EvaluateIntegerArithmetic(Opcode::ShiftLeftInt, 1, -1), kMinimum);
  EXPECT_EQ(EvaluateIntegerArithmetic(Opcode::ShiftRightInt, kMinimum, -1), -1);
  EXPECT_EQ(EvaluateIntegerArithmetic(Opcode::ShiftUnsignedRightInt, -1, kWidth + 1), kMaximum);
}

}  // namespace
}  // namespace vela
