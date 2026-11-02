#include <cstdint>
#include <limits>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

TEST(ConstantPoolTest, SupportsBothInt64LimitsAndFoldedLargeConstants) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(R"(
func minimum(): int { return -9223372036854775808; }
func maximum(): int { return 9223372036854775807; }
func folded(): int { return 2000000000 * 2000000000; }
)",
                                            isolate);
  ASSERT_TRUE(compiled.Succeeded());
  EXPECT_EQ(test::Execute(*compiled.instance, "minimum").value.AsInteger(),
            std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(test::Execute(*compiled.instance, "maximum").value.AsInteger(),
            std::numeric_limits<std::int64_t>::max());
  constexpr std::int64_t kFolded = 4000000000000000000;
  EXPECT_EQ(test::Execute(*compiled.instance, "folded").value.AsInteger(), kFolded);
}

TEST(ConstantPoolTest, PreservesLargeIntegerConstantsAcrossLoops) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileCleanSource(R"(
func main(): int {
  let total = 0;
  for (let index = 0; index < 2; index = index + 1) { total = total + 40000; }
  return total - 40000;
})",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(**compiled);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 40000);
}

TEST(ConstantPoolTest, ExecutesRadixLiteralsThroughConstantsPatternsAndIntegerOperations) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileCleanSource(R"(
const mask = 0XFF;
func masked(value: int): int { return value & mask; }
func main(): int {
  if (0x7fffffffffffffff != 9223372036854775807 ||
      -0x8000000000000000 != -9223372036854775808 ||
      0o777777777777777777777 != 9223372036854775807 ||
      -0o1000000000000000000000 != -9223372036854775808 ||
      0b111111111111111111111111111111111111111111111111111111111111111 !=
          9223372036854775807 ||
      -0b1000000000000000000000000000000000000000000000000000000000000000 !=
          -9223372036854775808) { return 0; }
  const values = [0x2A, 0B101010, 0O52, -0x2a, -0b101010, -0o52];
  if (values[0] != values[1] || values[1] != values[2] ||
      values[3] != values[4] || values[4] != values[5]) { return 0; }
  const label = match (masked(0x12a)) { 0b101010 => 0o52, _ => 0 };
  switch (label) {
    case 0x2a: return 0x2a.toString() == '42' ? label : 0;
    default: return 0;
  }
})",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(**compiled);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

class InvalidIntegerLiteralTest : public testing::TestWithParam<std::string_view> {};

TEST_P(InvalidIntegerLiteralTest, DiagnosesUnrepresentableLiteral) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(GetParam(), isolate);
  EXPECT_FALSE(compiled.Succeeded());
  ASSERT_FALSE(compiled.diagnostics.empty());
  EXPECT_EQ(compiled.diagnostics.front().message, "integer literal is out of range");
}

INSTANTIATE_TEST_SUITE_P(
    Bounds,
    InvalidIntegerLiteralTest,
    testing::Values("func main(): int { return 9223372036854775808; }",
                    "func main(): int { return -9223372036854775809; }",
                    "func main(): int { return 0x8000000000000000; }",
                    "func main(): int { return -0x8000000000000001; }",
                    "func main(): int { return 0xffffffffffffffff; }",
                    "func main(): int { return 0x10000000000000000; }",
                    "func main(): int { return 0o1000000000000000000000; }",
                    "func main(): int { return -0o1000000000000000000001; }",
                    "func main(): int { return 0b10000000000000000000000000000000"
                    "00000000000000000000000000000000; }",
                    "func main(): int { return -0b10000000000000000000000000000000"
                    "00000000000000000000000000000001; }"));

}  // namespace
}  // namespace vela::compiler
