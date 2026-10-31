#include <cmath>
#include <limits>
#include <string_view>

#include "base/execution_status.h"
#include "gtest/gtest.h"

#include "compiler/test_support/program_test_support.h"
#include "compiler/ir/preparation_test_support.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

constexpr double kRoundTripValue = 1.2345678901234567;

TEST(DoubleTest, ExecutesArithmeticComparisonsMethodsAndArrays) {
  constexpr std::string_view source = R"(
class Accumulator {
  value: double;
  add(amount: double): double {
    this.value = this.value + amount;
    return this.value;
  }
}
func values(value: double): double[] { return [value, 2.0]; }
func main(): bool {
  let sum: Accumulator = Accumulator(value: 1.25);
  let array: double[] = values(sum.add(0.25));
  let nested: double[][] = [[], array];
  nested[0] = [3.0];
  array[1] = array[0] * 4.0 - 1.0;
  let negative: double = -array[1];
  return array[0] == 1.5 && array[1] != 6.0 &&
         array[1] / 2.0 == 2.5 && negative == -5.0 &&
         negative < 0.0 && negative <= -5.0 &&
         array[1] > 4.0 && array[1] >= 5.0 &&
         1e2 == 100.0 && 1.25e-1 == 0.125 &&
         nested[0][0] == 3.0;
}
)";
  runtime::Isolate isolate(1);
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = test::Execute(**entry);
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsBoolean());
  EXPECT_TRUE(result.value.AsBoolean());
}

TEST(DoubleTest, DeduplicatesConstantsAndReturnsDouble) {
  runtime::Isolate isolate;
  const auto entry =
      test::CompileCleanSource("func main(): double { return 42.5 + 42.5 + 42.5; }", isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto prepared = ir::test::Prepare("func main(): double { return 42.5 + 42.5 + 42.5; }");
  ASSERT_TRUE(prepared.Succeeded());
  const auto& function = prepared.program.functions.front();
  ASSERT_EQ(function.doubles.size(), 1U);
  EXPECT_DOUBLE_EQ(function.doubles.front(), 42.5);
  const auto result = test::Execute(**entry);
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsDouble());
  EXPECT_DOUBLE_EQ(result.value.AsDouble(), 127.5);
}

struct DoubleConstantCase final {
  std::string_view source;
  double expected = 0.0;
};

class DoubleConstantTest : public testing::TestWithParam<DoubleConstantCase> {};

TEST_P(DoubleConstantTest, PreservesBinary64ConstantsAndSignedZero) {
  const auto& parameter = GetParam();
  SCOPED_TRACE(parameter.source);
  runtime::Isolate isolate;
  const auto entry = test::CompileCleanSource(parameter.source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = test::Execute(**entry);
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsDouble());
  const double actual = result.value.AsDouble();
  EXPECT_DOUBLE_EQ(actual, parameter.expected);
  EXPECT_EQ(std::signbit(actual), std::signbit(parameter.expected));
}

INSTANTIATE_TEST_SUITE_P(
    Binary64,
    DoubleConstantTest,
    testing::Values(
        DoubleConstantCase{
            .source = "func main(): double { return 1.7976931348623157e308; }",
            .expected = std::numeric_limits<double>::max(),
        },
        DoubleConstantCase{
            .source = "func main(): double { return 4.9406564584124654e-324; }",
            .expected = std::numeric_limits<double>::denorm_min(),
        },
        DoubleConstantCase{.source = "func main(): double { return -0.0; }", .expected = -0.0},
        DoubleConstantCase{
            .source = "func main(): double { return 1.2345678901234567; }",
            .expected = kRoundTripValue,
        }));

TEST(DoubleTest, PreservesIeeeComparisonsForNanAndInfinity) {
  constexpr std::string_view source = R"(
func main(): bool {
  let infinity: double = 1e308 * 1e308;
  let nan: double = infinity - infinity;
  return infinity > 1e308 && nan != nan && !(nan == nan) &&
         !(nan < 0.0) && !(nan <= 0.0) && !(nan > 0.0) && !(nan >= 0.0);
}
)";
  runtime::Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = test::Execute(**entry);
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsBoolean());
  EXPECT_TRUE(result.value.AsBoolean());
}

class DoubleDivisionByZeroTest : public testing::TestWithParam<std::string_view> {};

TEST_P(DoubleDivisionByZeroTest, ReportsDivisionByZero) {
  const auto source = GetParam();
  SCOPED_TRACE(source);
  runtime::Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  EXPECT_EQ(test::Execute(**entry).status,
            ExecutionStatus::DivisionByZero);
}

INSTANTIATE_TEST_SUITE_P(SignedZero,
                         DoubleDivisionByZeroTest,
                         testing::Values("func main(): double { return 1.0 / 0.0; }",
                                         "func main(): double { return 1.0 / -0.0; }"));

}  // namespace
}  // namespace vela::compiler
