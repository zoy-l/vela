#include <cstdint>
#include <limits>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/bytecode/compiler.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate/isolate.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

TEST(ConstantPoolTest, SupportsBothInt64LimitsAndFoldedLargeConstants) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(
func minimum(): int { return -9223372036854775808; }
func maximum(): int { return 9223372036854775807; }
func folded(): int { return 2000000000 * 2000000000; }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  vm::VirtualMachine machine(isolate);
  EXPECT_EQ(machine.Execute(compiled.program.FindFunction("minimum")).value.AsInteger(),
            std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(machine.Execute(compiled.program.FindFunction("maximum")).value.AsInteger(),
            std::numeric_limits<std::int64_t>::max());
  constexpr std::int64_t kFolded = 4000000000000000000;
  EXPECT_EQ(machine.Execute(compiled.program.FindFunction("folded")).value.AsInteger(), kFolded);
}

TEST(ConstantPoolTest, DeduplicatesLargeIntegersAndPreservesSmallImmediateFastPath) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileCleanSource(R"(
func main(): int {
  let total = 0;
  for (let index = 0; index < 2; index = index + 1) { total = total + 40000; }
  return total - 40000;
})",
                                                 isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto& function = compiled->functions().front();
  ASSERT_EQ(function.integers.size(), 1U);
  EXPECT_EQ(function.integers.front(), 40000);
  const auto result = test::ExecuteInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 40000);
}

class InvalidIntegerLiteralTest : public testing::TestWithParam<std::string_view> {};

TEST_P(InvalidIntegerLiteralTest, DiagnosesUnrepresentableLiteral) {
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(Bounds,
                         InvalidIntegerLiteralTest,
                         testing::Values("func main(): int { return 9223372036854775808; }",
                                         "func main(): int { return -9223372036854775809; }"));

}  // namespace
}  // namespace vela::compiler
