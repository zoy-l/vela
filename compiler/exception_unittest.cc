#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/frontend/checker.h"
#include "runtime/isolate.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class ExceptionExecutionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ExceptionExecutionTest, CompletesWithPreciseUnwindingAndStrictTypes) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(GetParam(), isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.source_path << ':' << diagnostic.location.line << ':'
                  << diagnostic.location.column << ' ' << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(Unwinding,
                         ExceptionExecutionTest,
                         testing::Values(
                             R"(
class Failure { param value: int; }
func main(): int {
  try { throw Failure(value: 42); } catch (error: Failure) { return error.value; }
})",
                             R"(
class First {}
class Second { param value: int; }
func main(): int {
  try { throw Second(value: 42); }
  catch (error: First) { return 1; }
  catch (error: Second) { return error.value; }
})",
                             R"(
func main(): int {
  try { let zero = 0; return 1 / zero; }
  catch (error: Error) {
    if (error.message == "division by zero" && error.code == "DivisionByZero") { return 42; }
    return 0;
  }
})",
                             R"(
class Failure {}
func main(): int { try { throw Failure(); } catch { return 42; } }
)",
                             R"(
func main(): int { let value = 42; try { return value; } finally { value = 1; } }
)",
                             R"(
let trace = 0;
func run(): int {
  try { try { return 40; } finally { trace = trace * 10 + 1; } }
  finally { trace = trace * 10 + 2; }
}
func main(): int { const value = run(); if (value == 40 && trace == 12) { return 42; } return 0; }
)",
                             R"(
func main(): int {
  try { try { return 1; } finally { return 2; } } finally { return 42; }
})",
                             R"(
func main(): int {
  let sum = 0; let index = 0;
  while (index < 5) {
    index = index + 1;
    try {
      if (index == 2) { continue; }
      if (index == 4) { break; }
      sum = sum + index;
    } finally { sum = sum + 10; }
  }
  return sum - 2;
})",
                             R"(
func main(): int {
  let value = 0;
  try { while (true) { break; } value = value + 1; } finally { value = value + 10; }
  return value + 31;
})",
                             R"(
func main(): int {
  let value = 0;
  while (true) { try { return 1; } finally { value = 42; break; } }
  return value;
})",
                             R"(
class First {}
class Second { param value: int; }
func main(): int {
  try {
    try { throw First(); } catch (error: First) { return 1; }
    finally { throw Second(value: 42); }
  } catch (error: Second) { return error.value; }
})",
                             R"(
class First {}
class Second {}
func main(): int {
  let value = 0;
  try {
    try { throw First(); } catch (error: First) { throw Second(); }
    finally { value = 40; }
  } catch (error: Second) { return value + 2; }
})",
                             R"(
class Failure { param message: string; }
func main(): int {
  try {
    try { throw Failure(message: "retained" + " payload"); }
    finally {
      let index = 0;
      while (index < 200) { const temporary = "allocated " + index.toString(); index = index + 1; }
    }
  } catch (error: Failure) { if (error.message == "retained payload") { return 42; } return 0; }
})",
                             R"(
class Failure { param value: int; }
func main(): int {
  let saved: () => int = (): int => 0;
  try { throw Failure(value: 1); }
  catch (error: Failure) { saved = (): int => error.value; error = Failure(value: 42); }
  return saved();
})",
                             R"(
class Failure<T> { param value: T; }
func run<T>(value: T): T {
  try { throw Failure<T>(value: value); } catch (error: Failure<T>) { return error.value; }
}
func main(): int { return run<int>(42); }
)",
                             R"(
class Counter {
  value: int;
  constructor() { try { return; } finally { this.value = 42; } }
}
func main(): int { return Counter().value; }
)",
                             R"(
func main(): int {
  let value = 40;
  try { let value = 1; return value + 41; } finally { value = value + 2; }
})",
                             R"(
func main(): int { try { return 42; } catch { return 0; } }
)",
                             R"(
func main(): int {
  let value = 0;
  try { value = 40; } finally { value = value + 2; }
  return value;
})",
                             R"(
func main(): int {
  let first = '';
  try { Math.sqrt(-1.0); }
  catch (error: Error) {
    if (error.code != 'NativeFailure') { return 0; }
    first = error.message;
  }
  try { Random.nextInt(0); }
  catch (error: Error) {
    if (error.code == 'NativeFailure' && error.message != first) { return 42; }
  }
  return 0;
})",
                             R"(
func main(): int {
  let caught = 0;
  try { const values = [1]; print(values[2]); }
  catch (error: Error) { if (error.code == 'IndexOutOfBounds') { caught += 1; } }
  try { const value: int? = null; print(value as int); }
  catch (error: Error) { if (error.code == 'InvalidCast') { caught += 1; } }
  try { throw Error(message: 'custom message', code: 'ApplicationFailure'); }
  catch (error: Error) {
    if (error.code == 'ApplicationFailure' && error.message == 'custom message') { caught += 1; }
  }
  if (caught == 3) { return 42; }
  return 0;
})"));

TEST(ExceptionTest, FinallyPreservesOriginalTraceAcrossNestedHandledExceptions) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(
      "class Failure {}\n"
      "func fail(): int { let zero = 0; return 1 / zero; }\n"
      "func main(): int {\n"
      "  try { return fail(); } finally { try { throw Failure(); } catch {} }\n"
      "}",
      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  EXPECT_EQ(result.status, vm::ExecutionStatus::DivisionByZero);
  ASSERT_EQ(result.stack_trace.size(), 2U);
  EXPECT_EQ(result.stack_trace.front().function, "fail");
  EXPECT_EQ(result.stack_trace.front().location.line, 2U);
  EXPECT_EQ(result.stack_trace.back().function, "main");
  EXPECT_EQ(result.stack_trace.back().location.line, 4U);
}

TEST(ExceptionTest, FailedGlobalInitializerUnwindsAndCanBeRetriedAfterCatch) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
class Failure {}
let attempts = 0;
let answer = initialize();
func initialize(): int {
  attempts = attempts + 1;
  if (attempts == 1) { throw Failure(); }
  return 42;
}
func main(): int {
  try { return answer; } catch (error: Failure) { return answer; }
})",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST(ExceptionTest, RepeatedCaughtFailuresReleaseTheirEnvelopesAfterExecution) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
class Failure {}
func main(): int {
  let index = 0;
  while (index < 200) {
    try { throw Failure(); } catch { index = index + 1; }
  }
  return 42;
})",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().allocated_bytes(), 0U);
}

class InvalidExceptionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(InvalidExceptionTest, RejectsInvalidSyntaxTypesAndUnsafeFlow) {
  EXPECT_FALSE(frontend::CheckSource(GetParam()).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    Invalid,
    InvalidExceptionTest,
    testing::Values(
        "func main(): void { throw 1; }",
        "func main(): void { throw null; }",
        "func main(): void { throw \"error\"; }",
        "func main(): void { try {} catch (error: int) {} }",
        "class E {} func main(): void { try {} catch (error: E?) {} }",
        "class E {} func main(): void { try {} catch {} catch (error: E) {} }",
        "class E {} func main(): void { try {} catch (a: E) {} catch (b: E) {} }",
        "func main(): void { try {} }",
        "func main(): void { try {} finally { break; } }",
        "class E {} func main(): int { let value: int? = 42; if (value != null) { "
        "try { value = null; throw E(); } catch { return value + 1; } } return 0; }",
        "class E {} class C { value: int; constructor() { try { throw E(); } catch {} } }"));

}  // namespace
}  // namespace vela::compiler
