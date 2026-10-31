#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "base/execution_status.h"
#include "gtest/gtest.h"

#include "compiler/test_support/program_test_support.h"
#include "compiler/test_support/test_sources.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_function.h"

namespace vela::compiler {
namespace {

class CollectionLanguageTest : public testing::TestWithParam<std::string_view> {};

TEST_P(CollectionLanguageTest, ExecutesTypedCollectionsUnderGcPressure) {
  runtime::Isolate isolate(1);
  const auto program = test::CompileCleanSource(GetParam(), isolate);
  ASSERT_TRUE(program) << program.error();
  const auto retained = isolate.heap().object_count();
  const auto result = test::ExecuteInteger(**program);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), retained);
}

INSTANTIATE_TEST_SUITE_P(OrderedStorage,
                         CollectionLanguageTest,
                         testing::Values(test::kMapLiteralSource,
                                         R"(
func main(): int {
  const values = Set<int>();
  values.add(40); values.add(1); values.add(2); values.delete(1); values.add(1);
  if (!values.add(40) && values.has(1) && values.size == 3) {
    const ordered = values.values();
    return ordered[0] + ordered[1];
  }
  return 0;
}
)",
                                         R"(
func main(): int {
  const values: Map<string, int> = Map();
  values.set(key: "a" + "b", value: 40);
  values.set("next", 2);
  values.set("ab", 41);
  if (values.size != 2 || !values.has("ab")) { return 0; }
  const first = values.get("ab");
  const missing = values.get("missing");
  const ordered = values.keys();
  if (first != null && missing == null && ordered[0] == "ab") { return first + 1; }
  return 0;
}
)",
                                         R"(
class Item { value: int; }
func main(): int {
  const key = Item(value: 40);
  const values = Map<Item, string>();
  values.set(key, "a" + "b");
  if (values.has(Item(value: 40))) { return 0; }
  const stored = values.get(key);
  if (stored != null) { return key.value + stored.length; }
  return 0;
}
)",
                                         R"(
func main(): int {
  const inner = Set<int>(); inner.add(42);
  const outer = Map<string, Set<int>>(); outer.set("items", inner);
  const nested: Map<string, Set<int>>[] = [outer];
  const copy = nested[0].get("items");
  if (copy != null) { return copy.values()[0]; }
  return 0;
}
)",
                                         R"(
func main(): int {
  const values = Map<int?, int?>();
  values.set(null, null); values.set(40, 42);
  if (!values.has(null) || values.get(null) != null) { return 0; }
  const result = values.get(40);
  values.clear();
  if (result != null && values.size == 0) { return result; }
  return 0;
}
)",
                                         R"(
func main(): int {
  const infinity: double = 1e308 * 1e308;
  const nan: double = infinity - infinity;
  const values = Set<double>();
  values.add(0.0); values.add(-0.0); values.add(nan); values.add(-nan);
  if (values.has(nan) && values.size == 2) { return 42; }
  return 0;
}
)",
                                         R"(
class Box { value: int; }
func main(): int {
  const values = Map<int, Box>();
  values.set(1, Box(value: 42));
  const result = values.get(1);
  if (result != null) { return result.value; }
  return 0;
}
)",
                                         R"(
func main(): int {
  const values = Map<int, int>(); values.set(1, 9); values.set(2, 30);
  let total = 0;
  for (const [key, value] in values) {
    total += key + value;
    values.clear(); values.set(100, 100);
  }
  return total;
}
)",
                                         R"(
func main(): int {
  const values = Set<int>(); values.add(1); values.add(41);
  let total = 0;
  for (const value in values) {
    total += value;
    values.clear(); values.add(100);
  }
  return total;
}
)"));

class CollectionTypeErrorTest : public testing::TestWithParam<std::string_view> {};

TEST(MapLiteralTest, KeepsMapAndKeyAliveAcrossSuspensionAndFailure) {
  runtime::Isolate isolate(1);
  const auto program = test::CompileCleanSource(test::kMapLiteralAsyncSource, isolate);
  ASSERT_TRUE(program) << program.error();
  const auto result = test::ExecuteFutureInteger(**program);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST(MapLiteralTest, GlobalLiteralInitializationRetriesAndDetectsCycles) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(test::kMapLiteralInitializationSource, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

std::int64_t FreezeMapHeap(runtime::NativeContext& context) noexcept {
  auto& heap = context.isolate().heap();
  heap.SetAllocationLimit(heap.allocated_bytes());
  return 1;
}

TEST(MapLiteralTest, AllocationAndInsertionFailuresStopAtTheirActualBoundaries) {
  runtime::Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<FreezeMapHeap>("freeze")));
  const auto program = test::CompileCleanSource(R"(
external func freeze(): int;
let calls = 0;
func key(): int { calls++; return 1; }
func item(): int { calls++; return 42; }
func allocate(): int { const map = {key(): item()}; return map.size; }
func insert(): int { const map = {freeze(): item(), key(): item()}; return map.size; }
func count(): int { return calls; }
)",
                                                isolate);
  ASSERT_TRUE(program) << program.error();
  EXPECT_EQ(test::Execute(**program, "count").value.AsInteger(), 0);
  auto& heap = isolate.heap();
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  heap.SetAllocationLimit(baseline);
  const auto allocation = test::Execute(**program, "allocate");
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  EXPECT_EQ(allocation.status, ExecutionStatus::OutOfMemory);
  EXPECT_EQ(test::Execute(**program, "count").value.AsInteger(), 0);
  const auto insertion = test::Execute(**program, "insert");
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  EXPECT_EQ(insertion.status, ExecutionStatus::OutOfMemory);
  EXPECT_EQ(test::Execute(**program, "count").value.AsInteger(), 1);
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), baseline);
}

TEST(MapLiteralTest, ImplicitDynamicChecksRunBeforeTheFollowingExpressions) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileSource(R"(
let calls = 0;
func item(): int { calls++; return 42; }
func checked(key: dynamic, value: dynamic): Map<int, int> { return {key: value, 2: item()}; }
func main(): int {
  try { checked('wrong', 42); } catch (error: Error) { }
  if (calls != 0) { return 0; }
  try { checked(1, 'wrong'); } catch (error: Error) { }
  return calls == 0 ? 42 : 0;
}
)",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  ASSERT_EQ(compiled.diagnostics.size(), 2U);
  const auto result = test::ExecuteInteger(*compiled.instance);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

TEST_P(CollectionTypeErrorTest, RejectsInvalidKeysValuesAndImplicitConversions) {
  runtime::Isolate isolate;
  EXPECT_FALSE(test::CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    StaticTypes,
    CollectionTypeErrorTest,
    testing::Values("func main(): void { const values = Set<int>(); values.add(1.0); }",
                    "func main(): void { const values = Map<int, int>(); values.set(1, 2.0); }",
                    "func main(): void { const values = Set(); }",
                    "func main(): void { const values = Map<int>(); }",
                    "func main(): void { const values = Set<void>(); }",
                    "func main(): void { const values: Set<double> = Set<int>(); }",
                    "func main(): int { const values = Map<int, int>(); return values.get(1); }",
                    "type MaybeSet = Set<int>?; func main(): void { const values = MaybeSet(); }"));

}  // namespace
}  // namespace vela::compiler
