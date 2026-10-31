#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"

#include "compiler/test_support/program_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"
#include "runtime/heap/rooted_value.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_execution.h"

namespace vela::compiler {
namespace {

void ExpectStringResult(const runtime::ExecutionResult& result, std::string_view expected) {
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
    const auto compiled = test::CompileSource(source, isolate);
    ASSERT_TRUE(compiled.Succeeded());
    heap.Collect();
    const auto baseline = heap.object_count();
    const auto constants = compiled.instance->image().strings.values();
    ASSERT_EQ(constants.size(), kConstants);
    for (std::size_t index = 0; index < constants.size(); ++index) {
      EXPECT_EQ(compiled.instance->constant(index).AsString()->value(), constants.subspan(index).front().text());
    }
    const auto result = test::Execute(*compiled.instance);
    ExpectStringResult(result, "literal0literal39");
    heap.Collect();
    EXPECT_EQ(heap.object_count(), baseline);
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(CompilerStringGcTest, InstanceTransfersAndCapturedFunctionsKeepCodeAndConstantsAlive) {
  runtime::Isolate isolate(1);
  std::unique_ptr<runtime::NativeInstance> retained;
  std::optional<runtime::RootedValue> function;
  {
    auto compiled = test::CompileSource(R"(
func identity(value: string): string { return value; }
func main(): string { return identity("vela"); }
func single(): string { return "single"; }
func capture(): () => string { return single; }
)", isolate);
    ASSERT_TRUE(compiled.Succeeded());
    const auto captured = test::Execute(*compiled.instance, "capture");
    ASSERT_TRUE(captured.Succeeded());
    function.emplace(isolate, captured.value);
    retained = std::move(compiled.instance);
  }
  auto moved = std::move(retained);
  auto standalone = function;
  function.reset();
  isolate.heap().Collect();
  ExpectStringResult(test::Execute(*moved), "vela");
  moved.reset();
  isolate.heap().Collect();
  {
    const auto callable = standalone->value();
    auto origin = runtime::NativeInstance::Borrow(*runtime::Heap::NativeEnvironment(*callable.AsObject()));
    ExpectStringResult(runtime::ExecuteNativeCallable(origin, callable), "single");
  }
  standalone.reset();
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
    const auto compiled = test::CompileSource(kSource, isolate);
    ASSERT_TRUE(compiled.Succeeded());
    const auto result = test::Execute(*compiled.instance);
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
  const auto compiled = test::CompileSource(R"(
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
  const auto constant_objects = heap.object_count();
  const std::string expected(128, 'x');
  ExpectStringResult(test::Execute(*compiled.instance), expected);
  heap.Collect();
  const auto settled_committed = heap.committed_bytes();
  constexpr int kExecutions = 100;
  for (int iteration = 0; iteration < kExecutions; ++iteration) {
    const auto result = test::Execute(*compiled.instance);
    ExpectStringResult(result, expected);
    heap.Collect();
    EXPECT_EQ(heap.object_count(), constant_objects);
    EXPECT_EQ(heap.allocated_bytes(), constant_bytes);
    EXPECT_EQ(heap.committed_bytes(), settled_committed);
  }
}

}  // namespace
}  // namespace vela::compiler
