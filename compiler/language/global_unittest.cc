#include <array>
#include <string_view>

#include "base/execution_status.h"
#include "gtest/gtest.h"

#include "compiler/test_support/program_test_support.h"
#include "compiler/frontend/modules/source_loader.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

TEST(GlobalTest, ImportsShareMutableStateAndConstantsKeepStrictTypes) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(
import "state.vela" as state;
import "reader.vela" as reader;
func main(): int {
  state.current = state.current + 1;
  return reader.read() + state.step;
}
)",
      },
      {
          .path = "state.vela",
          .text = R"(
export let current: int = 40;
export const step = 1;
)",
      },
      {
          .path = "reader.vela",
          .text = R"(
import "state.vela";
export func read(): int { return current; }
)",
      },
  });
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanModules(sources, "main.vela", isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(**compiled);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(GlobalTest, GlobalClosuresAndTheirCellsStayRootedAcrossExecutionsAndCollection) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileSource(R"(
const counter = makeCounter();
func makeCounter(): () => int {
  let value: int = 40;
  return (): int => { value = value + 1; return value; };
}
func main(): int { return counter(); }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto results = test::ExecuteRepeated(*compiled.instance);
  ASSERT_TRUE(results) << results.error();
  EXPECT_EQ(results->front(), 41);
  EXPECT_EQ(results->back(), 42);
}

TEST(GlobalTest, ExplicitTypesPermitForwardReferencesAndReportInitializationCycles) {
  runtime::Isolate isolate;
  const auto entry = test::CompileCleanSource(R"(
const first: int = second;
const second: int = first;
func main(): int { return first; }
)",
                                        isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = test::Execute(**entry);
  EXPECT_EQ(result.status, ExecutionStatus::CyclicInitialization);
}

TEST(GlobalTest, InferredInitializersFollowDependenciesAndLocalsShadowGlobals) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
const first = second + 1;
const second = 40;
func add(first: int): int { return first + 1; }
func main(): int { return add(first); }
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

class GlobalErrorTest : public testing::TestWithParam<std::string_view> {};

TEST_P(GlobalErrorTest, RejectsInvalidBindingsAndInitializerContexts) {
  runtime::Isolate isolate;
  EXPECT_FALSE(test::CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    StrictGlobals,
    GlobalErrorTest,
    testing::Values("const value = 1; func main(): void { value = 2; }",
                    "const value: double = 1; func main(): void {}",
                    "const first = second; const second = first; func main(): void {}",
                    "const operation: (int) => int = (x: int): int => later; const later = x;",
                    "const value = null; func main(): void {}",
                    "func main(): void {} const value = main();",
                    "const f = (x: int): int => x; func main(): int { return f(x: 1); }",
                    "const value = 1; class value {}"));

}  // namespace
}  // namespace vela::compiler
