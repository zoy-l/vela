#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/isolate.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

TEST(ExpectedTest, PreservesActivePayloadThroughArraysFieldsClosuresAndGc) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
func divide(value: int, denominator: int): Expected<int, string> {
  if (denominator == 0) { return err("division" + " by zero"); }
  return ok(value / denominator);
}
class Holder { public param result: Expected<int, string>; }
func capture(value: Expected<int, string>): () => string { return (): string => value.error(); }
func done(): Expected<void, string> { return ok(); }
func main(): int {
  const values: Expected<int, string>[] = [divide(84, 2), divide(1, 0)];
  const holder = Holder(result: values[0]);
  const text = capture(values[1]);
  const nested: Expected<Expected<int, string>, bool> = ok(ok(42));
  if (holder.result.hasValue() && done().hasValue() && nested.value().value() == 42 &&
      text() == "division by zero") { return holder.result.value(); }
  return 0;
}
)",
                                      isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ": " << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST(ExpectedTest, DistinguishesIdenticalSuccessAndErrorTypes) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(
func main(): int {
  const success: Expected<int, int> = ok(40);
  const failure: Expected<int, int> = err(2);
  return success.value() + failure.error();
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST(ExpectedTest, ReportsAccessToInactiveAlternative) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(
func main(): int { const result: Expected<int, string> = err("failure"); return result.value(); }
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  EXPECT_EQ(vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main")).status,
            vm::ExecutionStatus::InactiveExpectedAlternative);
}

class ExpectedErrorTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ExpectedErrorTest, RejectsImplicitConstructionAndInsufficientContext) {
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    StrictTypes,
    ExpectedErrorTest,
    testing::Values("func main(): void { const x = ok(1); }",
                    "func main(): Expected<int, string> { return 42; }",
                    "func main(): Expected<int, string> { return err(42); }",
                    "func main(): Expected<int, string> { return ok(42.0); }",
                    "func main(): Expected<int, string> { return ok(); }",
                    "func main(): Expected<void, string> { return ok(1); }",
                    "func main(): void { const x: Expected<int, string> = ok(1); if (x) {} }",
                    "func main(): void { const x: Expected<int, void> = ok(1); }"));

}  // namespace
}  // namespace vela::compiler
