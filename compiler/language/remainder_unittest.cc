#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "compiler/test_support/checker_test_support.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

struct RemainderExpression final {
  std::string_view source;
  std::int64_t expected;
};

class RemainderExecutionTest : public testing::TestWithParam<RemainderExpression> {};

TEST_P(RemainderExecutionTest, ExecutesSignedRemaindersWithMultiplicativePrecedence) {
  const auto& parameter = GetParam();
  SCOPED_TRACE(parameter.source);
  const std::string source =
      "func remainder(left: int, right: int): int { return left % right; }"
      "func dynamicRemainder(left: dynamic, right: dynamic): dynamic { return left % right; }"
      "func main(): int { return " +
      std::string(parameter.source) + "; }";
  runtime::Isolate isolate;
  const auto result = test::RunInteger(source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, parameter.expected);
}

INSTANTIATE_TEST_SUITE_P(
    IntegersAndPrecedence,
    RemainderExecutionTest,
    testing::Values(RemainderExpression{"remainder(43, 5)", 3},
                    RemainderExpression{"remainder(-43, 5)", -3},
                    RemainderExpression{"remainder(43, -5)", 3},
                    RemainderExpression{"remainder(-43, -5)", -3},
                    RemainderExpression{"remainder(-9223372036854775808, 1)", 0},
                    RemainderExpression{"remainder(9223372036854775807, 2)", 1},
                    RemainderExpression{"17 % 5 * 3", 6},
                    RemainderExpression{"17 * 5 % 6", 1},
                    RemainderExpression{"17 / 5 % 2", 1},
                    RemainderExpression{"17 % 5 / 2", 1},
                    RemainderExpression{"17 % 5 % 2", 0},
                    RemainderExpression{"17 + 5 % 3", 19},
                    RemainderExpression{"17 % 5 + 3", 5},
                    RemainderExpression{"(17 + 5) % 3", 1},
                    RemainderExpression{"dynamicRemainder(-43, 5) as int", -3}));

TEST(RemainderTest, EvaluatesConstantBindingsInSwitchAndMatchPatterns) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
func main(): int {
  const label = 17 % 5;
  const matched = match (-3) { -43 % 5 => 40, _ => 0 };
  switch (2) { case label: return matched + 2; default: return 0; }
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(RemainderTest, RejectsNonIntegerOperandsBeforeLowering) {
  for (const std::string_view source : {
           "func f(): double { return 5.0 % 2.0; }",
           "func f(): int { return 5 % 2.0; }",
           "func f(): int { return 5.0 % 2; }",
           "func f(): string { return 'five' % 'two'; }",
           "func f(): bool { return true % false; }",
           "func f<T extends double>(left: T, right: T): T { return left % right; }",
       }) {
    SCOPED_TRACE(source);
    const auto checked = frontend::CheckTestSource(source);
    EXPECT_FALSE(checked.Succeeded());
    ASSERT_FALSE(checked.diagnostics.empty());
    EXPECT_EQ(checked.diagnostics.front().message, "remainder operands must be int");
  }
}

TEST(RemainderTest, RequiresNullableOperandsToBeCheckedForNull) {
  for (const std::string_view source : {
           "func f(value: int?): int { return value % 2; }",
           "func f(value: int?): int { return 5 % value; }",
       }) {
    SCOPED_TRACE(source);
    const auto checked = frontend::CheckTestSource(source);
    EXPECT_FALSE(checked.Succeeded());
    ASSERT_FALSE(checked.diagnostics.empty());
    EXPECT_EQ(checked.diagnostics.front().message,
              "'value' may be null; check for null before this operation");
  }
}

struct RemainderFailure final {
  std::string_view expression;
  ExecutionStatus status;
};

class RemainderFailureTest : public testing::TestWithParam<RemainderFailure> {};

TEST_P(RemainderFailureTest, PreservesErrorsEvenWhenTheResultIsUnused) {
  const auto& parameter = GetParam();
  SCOPED_TRACE(parameter.expression);
  const std::string source =
      "func remainder(left: int, right: int): int { return left % right; }"
      "func dynamicRemainder(left: dynamic, right: dynamic): dynamic { return left % right; }"
      "func main(): int { const unused = " +
      std::string(parameter.expression) + "; return 42; }";
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  EXPECT_EQ(test::Execute(*compiled.instance).status, parameter.status);
}

INSTANTIATE_TEST_SUITE_P(
    CheckedIntegerSemantics,
    RemainderFailureTest,
    testing::Values(RemainderFailure{"43 % 0", ExecutionStatus::DivisionByZero},
                    RemainderFailure{"remainder(43, 0)", ExecutionStatus::DivisionByZero},
                    RemainderFailure{"-9223372036854775808 % -1", ExecutionStatus::IntegerOverflow},
                    RemainderFailure{"remainder(-9223372036854775808, -1)",
                                     ExecutionStatus::IntegerOverflow},
                    RemainderFailure{"dynamicRemainder(43, 0)", ExecutionStatus::DivisionByZero},
                    RemainderFailure{"dynamicRemainder(-9223372036854775808, -1)",
                                     ExecutionStatus::IntegerOverflow},
                    RemainderFailure{"dynamicRemainder(5.0, 2.0)", ExecutionStatus::TypeError},
                    RemainderFailure{"dynamicRemainder('five', 2)", ExecutionStatus::TypeError}));

}  // namespace
}  // namespace vela::compiler
