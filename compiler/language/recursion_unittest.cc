#include "gtest/gtest.h"

#include "compiler/test_support/program_test_support.h"
#include "compiler/test_support/test_sources.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

TEST(RecursionTest, ExecutesDirectAndMutualRecursion) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
func factorial(value: int): int { if (value == 0) return 1; return value * factorial(value - 1); }
func even(value: int): bool { if (value == 0) return true; return odd(value - 1); }
func odd(value: int): bool { if (value == 0) return false; return even(value - 1); }
func main(): int { if (even(20) && odd(21)) return factorial(6); return 0; }
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 720);
}

TEST(RecursionTest, ReportsStackOverflowAndReusesInstanceAfterFailure) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(R"(
func recursive(): int { return recursive(); }
func main(): int { return 42; }
)",
                                            isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto baseline = isolate.heap().object_count();
  EXPECT_EQ(test::Execute(*compiled.instance, "recursive").status, ExecutionStatus::StackOverflow);
  const auto result = test::Execute(*compiled.instance, "main");
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), baseline);
}

TEST(RecursionTest, KeepsDeepCallFramesStableAcrossAllocationsAndCollection) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanSource(test::kRecursiveStringGcSource, isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto baseline = isolate.heap().object_count();
  const auto result = test::ExecuteInteger(**compiled);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), baseline);
}

}  // namespace
}  // namespace vela::compiler
