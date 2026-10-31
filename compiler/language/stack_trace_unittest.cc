#include <algorithm>
#include <array>
#include <sstream>
#include <string>

#include "gtest/gtest.h"

#include "compiler/bytecode/compiler.h"
#include "compiler/frontend/modules/source_loader.h"
#include "runtime/isolate/isolate.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

TEST(StackTraceTest, CapturesOriginalErrorAndCallerLocationsAcrossModules) {
  runtime::Isolate isolate;
  vm::ExecutionResult result;
  {
    const auto sources = std::to_array<frontend::SourceInput>({
        {
            .path = "main.vela",
            .text = "import \"lib.vela\" as lib;\nfunc main(): int {\n  return lib.run();\n}\n",
        },
        {
            .path = "lib.vela",
            .text = "func divide(value: int): int {\n  return 10 / value;\n}\n"
                    "export func run(): int {\n  return divide(0);\n}\n",
        },
    });
    const auto compiled = CompileModules(sources, "main.vela", isolate);
    ASSERT_TRUE(compiled.Succeeded());
    result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  }
  EXPECT_EQ(result.status, vm::ExecutionStatus::DivisionByZero);
  ASSERT_EQ(result.stack_trace.size(), 3U);
  const auto& origin = result.stack_trace.front();
  EXPECT_EQ(origin.source_path, "lib.vela");
  EXPECT_EQ(origin.location.line, 2U);
  EXPECT_EQ(result.stack_trace.at(1).location.line, 5U);
  const auto& entry = result.stack_trace.back();
  EXPECT_EQ(entry.function, "main");
  EXPECT_EQ(entry.source_path, "main.vela");
  EXPECT_EQ(entry.location.line, 3U);
  std::ostringstream output;
  vm::WriteRuntimeError(output, result);
  const auto text = output.str();
  EXPECT_TRUE(text.starts_with("lib.vela:2:"));
  EXPECT_NE(text.find(": error: division by zero\n"), std::string::npos);
  EXPECT_NE(text.find("  at main (main.vela:3:"), std::string::npos);
}

TEST(StackTraceTest, SuccessfulExecutionHasNoTraceAndMachineCanBeReusedAfterError) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(
      "func fail(): int { let zero = 0; return 1 / zero; }\n"
      "func main(): int { return 42; }",
      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  vm::VirtualMachine machine(isolate);
  const auto failed = machine.Execute(compiled.program.FindFunction("fail"));
  ASSERT_EQ(failed.status, vm::ExecutionStatus::DivisionByZero);
  ASSERT_EQ(failed.stack_trace.size(), 1U);
  const auto succeeded = machine.Execute(compiled.program.FindFunction("main"));
  EXPECT_TRUE(succeeded.Succeeded());
  EXPECT_TRUE(succeeded.stack_trace.empty());
  EXPECT_EQ(succeeded.value.AsInteger(), 42);
}

TEST(StackTraceTest, RecursiveOverflowCapturesAllLiveFrames) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource("func main(): int { return main(); }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  EXPECT_EQ(result.status, vm::ExecutionStatus::StackOverflow);
  EXPECT_GT(result.stack_trace.size(), 1U);
  EXPECT_TRUE(std::ranges::all_of(result.stack_trace, [](const auto& frame) -> bool {
    return frame.function == "main" && frame.source_path == "main.vela" && frame.location.line == 1;
  }));
}

}  // namespace
}  // namespace vela::compiler
