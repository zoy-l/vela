#include <algorithm>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/compiler.h"
#include "compiler/frontend/diagnostic.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class DynamicConversionDiagnosticTest : public testing::TestWithParam<std::string_view> {};

TEST_P(DynamicConversionDiagnosticTest, WarnsWithoutDisablingChecksAndCanBeAnError) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(GetParam(), isolate);
  ASSERT_TRUE(compiled.Succeeded());
  ASSERT_TRUE(std::ranges::any_of(compiled.diagnostics, [](const auto& diagnostic) {
    return diagnostic.code == frontend::DiagnosticCode::ImplicitDynamicConversion &&
           diagnostic.severity == frontend::DiagnosticSeverity::Warning;
  }));
  const auto rejected = CompileSource(
      GetParam(), isolate, {.implicit_dynamic_conversion = frontend::DiagnosticSeverity::Error});
  EXPECT_FALSE(rejected.Succeeded());
  EXPECT_TRUE(frontend::HasErrors(rejected.diagnostics));
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries,
    DynamicConversionDiagnosticTest,
    testing::Values(
        "func main(): void { const data: dynamic = 42; const value: int = data; }",
        "func main(): void { const data: dynamic = 42; let value = 0; value = data; }",
        "func use(value: int):void {} func main():void { const data:dynamic=42; use(data); }",
        "func main(): int { const data: dynamic = 42; return data; }",
        "const data: dynamic = 42; const value: int = data; func main():void {}",
        "class Box { value: int = Json.parse('42'); } func main():void { Box(); }",
        "func main():void { const data:dynamic=42; const values:int[]=[data]; }",
        "func main():void { const data:dynamic=42; const values=[0]; values[0]=data; }",
        "func main():void { const data:dynamic=true; if(data) {} }",
        "async func main():Future<int> { return Json.parse('42'); }",
        "func main():void { const value:dynamic=42; const result:Future<int> = "
        "Future.value(value); }",
        "func main():void { const values:dynamic=[Future.value(42)]; "
        "const result=Future.wait<int>(values); }",
        "func main():void { const root = Json.parse('{}'); const name:string=root.get('name') ?? "
        "''; }"));

TEST(DynamicConversionDiagnosticTest, ExplicitChecksAndValidRefinementsDoNotWarn) {
  runtime::Isolate isolate;
  const auto compiled =
      CompileSource(R"(
func main(): int {
  const value: dynamic = Json.parse('42');
  const erased: dynamic = value;
  const checked = erased as int;
  if (value is int) { const precise: int = value; return precise + checked - 42; }
  return 0;
}
)",
                    isolate, {.implicit_dynamic_conversion = frontend::DiagnosticSeverity::Error});
  ASSERT_TRUE(compiled.Succeeded());
  EXPECT_TRUE(compiled.diagnostics.empty());
  const auto executed = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(executed.Succeeded());
  EXPECT_EQ(executed.value.AsInteger(), 42);
}

TEST(DynamicConversionDiagnosticTest, ImplicitConversionStillThrowsAndPreservesCatchBehavior) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(
func main(): int {
  const value: dynamic = 'wrong';
  try { const number: int = value; return number; }
  catch (error: Error) { return 42; }
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  ASSERT_EQ(compiled.diagnostics.size(), 1U);
  const auto executed = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(executed.Succeeded()) << executed.message;
  EXPECT_EQ(executed.value.AsInteger(), 42);
}

TEST(DynamicConversionDiagnosticTest, ReportsEachTypedBindingOfACachedDynamicResultAdapter) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(
func main(): void {
  const values = Map<string, dynamic>();
  const first: (string) => int = values.get;
  const second: (string) => int = values.get;
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  ASSERT_EQ(compiled.diagnostics.size(), 2U);
  EXPECT_NE(compiled.diagnostics.front().location, compiled.diagnostics.back().location);
}

}  // namespace
}  // namespace vela::compiler
