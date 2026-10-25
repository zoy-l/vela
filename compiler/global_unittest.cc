#include <array>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/frontend/source_loader.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

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
  const auto compiled = CompileModules(sources, "main.vela", isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST(GlobalTest, GlobalClosuresAndTheirCellsStayRootedAcrossExecutionsAndProgramCopies) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
const counter = makeCounter();
func makeCounter(): () => int {
  let value: int = 40;
  return (): int => { value = value + 1; return value; };
}
func main(): int { return counter(); }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto copied = compiled.program;
  vm::VirtualMachine machine(isolate);
  const auto first = machine.Execute(copied.FindFunction("main"));
  ASSERT_TRUE(first.Succeeded());
  EXPECT_EQ(first.value.AsInteger(), 41);
  isolate.heap().Collect();
  const auto second = machine.Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(second.Succeeded());
  EXPECT_EQ(second.value.AsInteger(), 42);
}

TEST(GlobalTest, ExplicitTypesPermitForwardReferencesAndReportInitializationCycles) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(
const first: int = second;
const second: int = first;
func main(): int { return first; }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  EXPECT_EQ(result.status, vm::ExecutionStatus::CyclicInitialization);
}

TEST(GlobalTest, InferredInitializersFollowDependenciesAndLocalsShadowGlobals) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(
const first = second + 1;
const second = 40;
func add(first: int): int { return first + 1; }
func main(): int { return add(first); }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

class GlobalErrorTest : public testing::TestWithParam<std::string_view> {};

TEST_P(GlobalErrorTest, RejectsInvalidBindingsAndInitializerContexts) {
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource(GetParam(), isolate).Succeeded());
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
