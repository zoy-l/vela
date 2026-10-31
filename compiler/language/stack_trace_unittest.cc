#include <algorithm>
#include <array>
#include <sstream>
#include <string>

#include "gtest/gtest.h"

#include "compiler/test_support/program_test_support.h"
#include "compiler/frontend/modules/source_loader.h"
#include "runtime/isolate/isolate.h"
#include "runtime/process/process_entry.h"

namespace vela::compiler {
namespace {

TEST(StackTraceTest, CapturesOriginalErrorAndCallerLocationsAcrossModules) {
  runtime::Isolate isolate;
  runtime::ExecutionResult result;
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
    const auto compiled = test::CompileModules(sources, "main.vela", isolate);
    ASSERT_TRUE(compiled.Succeeded());
    result = test::Execute(*compiled.instance, "main");
  }
  EXPECT_EQ(result.status, ExecutionStatus::DivisionByZero);
  ASSERT_EQ(result.stack_trace.size(), 3U);
  const auto& origin = result.stack_trace.front();
  EXPECT_EQ(origin.source_path, "lib.vela");
  EXPECT_EQ(origin.line, 2U);
  EXPECT_EQ(result.stack_trace.at(1).line, 5U);
  const auto& entry = result.stack_trace.back();
  EXPECT_EQ(entry.function, "main");
  EXPECT_EQ(entry.source_path, "main.vela");
  EXPECT_EQ(entry.line, 3U);
  std::ostringstream output;
  runtime::WriteRuntimeDiagnostic(output, {.message = result.message, .stack_trace = result.stack_trace, .status = result.status, .origin = result.origin});
  const auto text = output.str();
  EXPECT_TRUE(text.starts_with("lib.vela:2:"));
  EXPECT_NE(text.find(": error: division by zero\n"), std::string::npos);
  EXPECT_NE(text.find("  at main (main.vela:3:"), std::string::npos);
}

TEST(StackTraceTest, SuccessfulExecutionHasNoTraceAndInstanceCanBeReusedAfterError) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(
      "func fail(): int { let zero = 0; return 1 / zero; }\n"
      "func main(): int { return 42; }",
      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto failed = test::Execute(*compiled.instance, "fail");
  ASSERT_EQ(failed.status, ExecutionStatus::DivisionByZero);
  ASSERT_EQ(failed.stack_trace.size(), 1U);
  const auto succeeded = test::Execute(*compiled.instance, "main");
  EXPECT_TRUE(succeeded.Succeeded());
  EXPECT_TRUE(succeeded.stack_trace.empty());
  EXPECT_EQ(succeeded.value.AsInteger(), 42);
}

TEST(StackTraceTest, RecursiveOverflowCapturesAllLiveFrames) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource("func main(): int { return main(); }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = test::Execute(*compiled.instance, "main");
  EXPECT_EQ(result.status, ExecutionStatus::StackOverflow);
  EXPECT_GT(result.stack_trace.size(), 1U);
  EXPECT_TRUE(std::ranges::all_of(result.stack_trace, [](const auto& frame) -> bool {
    return frame.function == "main" && frame.source_path == "main.vela" && frame.line == 1;
  }));
}

}  // namespace
}  // namespace vela::compiler
