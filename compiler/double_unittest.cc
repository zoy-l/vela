#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

constexpr double kRoundTripValue = 1.2345678901234567;

TEST(DoubleTest, ExecutesArithmeticComparisonsMethodsAndArrays) {
  constexpr std::string_view source = R"(
class Accumulator {
  double value;
  double add(double amount) {
    this.value = this.value + amount;
    return this.value;
  }
}
double[] values(double value) { return [value, 2.0]; }
bool main() {
  Accumulator sum = Accumulator(value: 1.25);
  double[] array = values(sum.add(0.25));
  double[][] nested = [[], array];
  nested[0] = [3.0];
  array[1] = array[0] * 4.0 - 1.0;
  double negative = -array[1];
  return array[0] == 1.5 && array[1] != 6.0 &&
         array[1] / 2.0 == 2.5 && negative == -5.0 &&
         negative < 0.0 && negative <= -5.0 &&
         array[1] > 4.0 && array[1] >= 5.0 &&
         1e2 == 100.0 && 1.25e-1 == 0.125 &&
         nested[0][0] == 3.0;
}
)";
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(source, isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ": " << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto* entry = compiled.program.FindFunction("main");
  ASSERT_NE(entry, nullptr);
  const auto index = static_cast<std::uint32_t>(entry - compiled.program.functions.data());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.View(), index);
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsBoolean());
  EXPECT_TRUE(result.value.AsBoolean());
}

TEST(DoubleTest, DeduplicatesConstantsAndReturnsDouble) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource("double main() { return 42.5 + 42.5 + 42.5; }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* entry = compiled.program.FindFunction("main");
  ASSERT_NE(entry, nullptr);
  ASSERT_EQ(entry->doubles.size(), 1U);
  EXPECT_DOUBLE_EQ(entry->doubles.front(), 42.5);
  const auto result = vm::VirtualMachine(isolate).Execute(entry->View());
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
  const auto compiled = CompileSource(parameter.source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* entry = compiled.program.FindFunction("main");
  ASSERT_NE(entry, nullptr);
  const auto result = vm::VirtualMachine(isolate).Execute(entry->View());
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsDouble());
  const double actual = result.value.AsDouble();
  EXPECT_DOUBLE_EQ(actual, parameter.expected);
  EXPECT_EQ(std::signbit(actual), std::signbit(parameter.expected));
}

INSTANTIATE_TEST_SUITE_P(Binary64,
                         DoubleConstantTest,
                         testing::Values(
                             DoubleConstantCase{
                                 .source = "double main() { return 1.7976931348623157e308; }",
                                 .expected = std::numeric_limits<double>::max(),
                             },
                             DoubleConstantCase{
                                 .source = "double main() { return 4.9406564584124654e-324; }",
                                 .expected = std::numeric_limits<double>::denorm_min(),
                             },
                             DoubleConstantCase{.source = "double main() { return -0.0; }",
                                                .expected = -0.0},
                             DoubleConstantCase{
                                 .source = "double main() { return 1.2345678901234567; }",
                                 .expected = kRoundTripValue,
                             }));

TEST(DoubleTest, PreservesIeeeComparisonsForNanAndInfinity) {
  constexpr std::string_view source = R"(
bool main() {
  double infinity = 1e308 * 1e308;
  double nan = infinity - infinity;
  return infinity > 1e308 && nan != nan && !(nan == nan) &&
         !(nan < 0.0) && !(nan <= 0.0) && !(nan > 0.0) && !(nan >= 0.0);
}
)";
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* entry = compiled.program.FindFunction("main");
  ASSERT_NE(entry, nullptr);
  const auto result = vm::VirtualMachine(isolate).Execute(entry->View());
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsBoolean());
  EXPECT_TRUE(result.value.AsBoolean());
}

class DoubleDivisionByZeroTest : public testing::TestWithParam<std::string_view> {};

TEST_P(DoubleDivisionByZeroTest, ReportsDivisionByZero) {
  const auto source = GetParam();
  SCOPED_TRACE(source);
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* entry = compiled.program.FindFunction("main");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(vm::VirtualMachine(isolate).Execute(entry->View()).status,
            vm::ExecutionStatus::DivisionByZero);
}

INSTANTIATE_TEST_SUITE_P(SignedZero,
                         DoubleDivisionByZeroTest,
                         testing::Values("double main() { return 1.0 / 0.0; }",
                                         "double main() { return 1.0 / -0.0; }"));

}  // namespace
}  // namespace vela::compiler
