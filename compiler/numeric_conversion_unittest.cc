#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/isolate.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

TEST(NumericConversionTest, ConvertsExplicitlyWithoutAllocating) {
  constexpr std::string_view source = R"(
func main(): bool {
  const positive: double = 42.9;
  const negative: double = -42.9;
  const minimum: double = -9223372036854775808.0;
  return positive.toInt() == 42 && negative.toInt() == -42 &&
         minimum.toInt().toDouble() == minimum &&
         42.toInt() == 42 && 42.toDouble() == 42.0 &&
         positive.toDouble() == positive && (-0.0).toInt() == 0 &&
         32767.toDouble() == 32767.0;
}
)";
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(source, isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto before = isolate.heap().allocated_bytes();
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsBoolean());
  EXPECT_TRUE(result.value.AsBoolean());
  EXPECT_EQ(isolate.heap().allocated_bytes(), before);
}

class InvalidNumericConversionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(InvalidNumericConversionTest, RejectsNonFiniteAndOutOfRangeDoubles) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(GetParam(), isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  EXPECT_EQ(result.status, vm::ExecutionStatus::InvalidNumericConversion);
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries,
    InvalidNumericConversionTest,
    testing::Values(
        "func main(): int { return 9223372036854775808.0.toInt(); }",
        "func main(): int { return (-9223372036854777856.0).toInt(); }",
        "func main(): int { const x: double = 1e308 * 1e308; return x.toInt(); }",
        "func main(): int { const x: double = -1e308 * 1e308; return x.toInt(); }",
        "func main(): int { const x: double = 1e308 * 1e308; return (x - x).toInt(); }"));

class NumericTypeBoundaryTest : public testing::TestWithParam<std::string_view> {};

TEST_P(NumericTypeBoundaryTest, RejectsImplicitConversionsAndInvalidReceivers) {
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    StrictTypes,
    NumericTypeBoundaryTest,
    testing::Values("func main(): double { return 42; }",
                    "func main(): int { return 42.0; }",
                    "func main(): double { return 1 + 2.0; }",
                    "func main(): int { return true.toInt(); }",
                    "func main(): double { return false.toDouble(); }",
                    "func main(): int { const x: double? = null; return x.toInt(); }"));

}  // namespace
}  // namespace vela::compiler
