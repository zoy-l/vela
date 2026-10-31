#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"

#include "compiler/bytecode/compiler.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

void ExpectStringResult(const vm::ExecutionResult& result, std::string_view expected) {
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsString());
  EXPECT_EQ(result.value.AsString()->value(), expected);
}

TEST(CompilerStringGcTest, RootsConstantsDuringCompilationAndBetweenExecutions) {
  runtime::Isolate isolate(1);
  auto& heap = isolate.heap();
  {
    std::string source;
    constexpr std::size_t kConstants = 40;
    for (std::size_t index = 0; index < kConstants; ++index) {
      const auto suffix = std::to_string(index);
      source.append("func s").append(suffix).append("(): string { return \"literal");
      source.append(suffix).append("\"; }\n");
    }
    source += "func main(): string { return s0() + s39(); }";
    const auto compiled = CompileSource(source, isolate);
    ASSERT_TRUE(compiled.Succeeded());
    heap.Collect();
    EXPECT_EQ(heap.object_count(), kConstants);
    const auto result = vm::VirtualMachine(isolate).Execute(compiled.program, kConstants);
    ExpectStringResult(result, "literal0literal39");
    heap.Collect();
    EXPECT_EQ(heap.object_count(), kConstants);
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(CompilerStringGcTest, CopiesAndMovesKeepOwnedPoolsAndViewsAlive) {
  runtime::Isolate isolate(1);
  vm::Program copy;
  vm::FunctionHandle function;
  {
    const auto compiled = CompileSource(R"(
func identity(value: string): string { return value; }
func main(): string { return identity("vela"); }
func single(): string { return "single"; }
)",
                                        isolate);
    ASSERT_TRUE(compiled.Succeeded());
    copy = compiled.program;
    function = compiled.program.FindFunction("single");
  }
  vm::Program moved(std::move(copy));
  vm::Program constructed(moved);
  moved = vm::Program();
  auto standalone = std::move(function);
  isolate.heap().Collect();
  const auto result = vm::VirtualMachine(isolate).Execute(constructed, 1);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsString()->value(), "vela");
  constructed = vm::Program();
  isolate.heap().Collect();
  const auto single = vm::VirtualMachine(isolate).Execute(standalone);
  ASSERT_TRUE(single.Succeeded());
  EXPECT_EQ(single.value.AsString()->value(), "single");
  standalone = vm::FunctionHandle();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

TEST(CompilerStringGcTest, ReturnedFieldsAndNestedArraysTraceDynamicStrings) {
  runtime::Isolate isolate(1);
  auto& heap = isolate.heap();
  runtime::Value value = runtime::Value::Null();
  runtime::RootRange root(heap, &value, 1);
  {
    constexpr std::string_view kSource = R"(
class Store { label: string; rows: string[][]; }
func combine(left: string, right: string): string { return left + right; }
func main(): Store {
  return Store(label: combine("a", "b"), rows: [[combine("c", "d")]]);
}
)";
    const auto compiled = CompileSource(kSource, isolate);
    ASSERT_TRUE(compiled.Succeeded());
    const auto result = vm::VirtualMachine(isolate).Execute(compiled.program, 1);
    ASSERT_TRUE(result.Succeeded());
    value = result.value;
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 5U);
  const auto* label = runtime::Heap::GetField(*value.AsObject(), 0);
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->AsString()->value(), "ab");
  const auto* rows = runtime::Heap::GetField(*value.AsObject(), 1);
  ASSERT_NE(rows, nullptr);
  const auto row = runtime::Heap::GetElement(*rows->AsObject(), 0);
  ASSERT_TRUE(row.has_value());
  const auto element = runtime::Heap::GetElement(*row->AsObject(), 0);
  ASSERT_TRUE(element.has_value());
  EXPECT_EQ(element->AsString()->value(), "cd");
  value = runtime::Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(CompilerStringGcTest, RepeatedConcatenationHasBoundedRetainedStorage) {
  runtime::Isolate isolate(1);
  auto& heap = isolate.heap();
  const auto compiled = CompileSource(R"(
func append(value: string): string { return value + "x"; }
func main(): string {
  let text: string = "";
  for (let i: int = 0; i < 128; i = i + 1) { text = append(text); }
  return text;
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  heap.Collect();
  const auto constant_bytes = heap.allocated_bytes();
  const std::string expected(128, 'x');
  ExpectStringResult(vm::VirtualMachine(isolate).Execute(compiled.program, 1), expected);
  heap.Collect();
  const auto settled_committed = heap.committed_bytes();
  constexpr int kExecutions = 100;
  for (int iteration = 0; iteration < kExecutions; ++iteration) {
    const auto result = vm::VirtualMachine(isolate).Execute(compiled.program, 1);
    ExpectStringResult(result, expected);
    heap.Collect();
    EXPECT_EQ(heap.object_count(), 2U);
    EXPECT_EQ(heap.allocated_bytes(), constant_bytes);
    EXPECT_EQ(heap.committed_bytes(), settled_committed);
  }
}

}  // namespace
}  // namespace vela::compiler
