#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/program_test_support.h"
#include "compiler/test_sources.h"
#include "runtime/isolate.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

TEST(RecursionTest, ExecutesDirectAndMutualRecursionWithExplicitVmFrames) {
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

TEST(RecursionTest, ReportsStackOverflowAndReusesVmAfterFailure) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(
func recursive(): int { return recursive(); }
func main(): int { return 42; }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  vm::VirtualMachine machine(isolate);
  EXPECT_EQ(machine.Execute(compiled.program.FindFunction("recursive")).status,
            vm::ExecutionStatus::StackOverflow);
  const auto result = machine.Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

TEST(RecursionTest, KeepsDeepCallFramesStableAcrossAllocationsAndCollection) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanSource(test::kRecursiveStringGcSource, isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(*compiled, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

}  // namespace
}  // namespace vela::compiler
