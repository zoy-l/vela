#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "base/execution_status.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/value.h"
#include "runtime/native/native_execution.h"

namespace vela::compiler {
namespace {

constexpr std::int64_t kExpectedSum = 42;
constexpr int kTemporaryAllocationCount = 32;

using vela::runtime::Isolate;
using vela::runtime::RootRange;
using vela::runtime::Value;

void AllocateAndCollectTemporaryStrings(Isolate& isolate) {
  for (int index = 0; index < kTemporaryAllocationCount; ++index) {
    static_cast<void>(isolate.heap().AllocateString("temporary"));
    isolate.heap().Collect();
  }
}

TEST(CompilerTest, CompilesQualifiedEnumMemberWithoutExpectedType) {
  constexpr std::string_view source = R"(
enum Shade { light, dark }
func main(): Shade {
  const value = Shade.dark;
  return value;
}
)";
  Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = test::Execute(**entry);
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsObject());
  const auto* layout = runtime::Heap::ClassOf(*result.value.AsObject());
  ASSERT_NE(layout, nullptr);
  EXPECT_TRUE(layout->IsEnum());
  EXPECT_EQ(runtime::Heap::GetField(*result.value.AsObject(), 0)->AsInteger(), 1);
}

TEST(CompilerTest, CompilesIntegerFunction) {
  constexpr std::string_view source = R"(
func main(): int {
  let x: int = 20;
  let y: int = 22;
  return x + y;
}
)";
  Isolate isolate;
  const auto result = test::RunInteger(source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kExpectedSum);
}

TEST(CompilerTest, NullSafeAccessCoalescesAndSkipsArgumentsAndIndices) {
  constexpr std::string_view source = R"(
class Item {
  value: int;
  read(unused: int): int { return this.value; }
}
func fail(): int { return 1 / 0; }
func main(): int {
  const missing: Item? = null;
  const present: Item? = Item(value: 20);
  const absent: int[]? = null;
  const values: int[]? = [22];
  const field: int? = present?.value;
  const method: int? = missing?.read(fail());
  const skipped: int? = absent?.[fail()];
  const indexed: int? = values?.[0];
  return (field ?? fail()) + (indexed ?? fail()) +
         (missing?.value ?? 0) + (present?.read(0) ?? fail()) - 20 +
         (method ?? 0) + (skipped ?? 0);
}
)";
  Isolate isolate;
  const auto result = test::RunInteger(source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kExpectedSum);
}

TEST(CompilerTest, CoalescePreservesStorageAndSkipsUnneededDefaults) {
  constexpr std::string_view source = R"(
let fallback_calls: int = 0;
let global_value: int? = null;
class Box { value: int?; }
func fallback(): int {
  fallback_calls += 1;
  return 42;
}
func main(): bool {
  let missing: int? = null;
  let present: int? = 7;
  const box = Box(value: null);
  const values: int?[] = [null, 7];
  const local_result: int = missing ?? fallback();
  const global_result: int = global_value ?? fallback();
  const field_result: int = box.value ?? fallback();
  const element_result: int = values[0] ?? fallback();
  const present_result: int = present ?? fallback();
  const present_element: int = values[1] ?? fallback();
  return local_result == 42 && global_result == 42 && field_result == 42 &&
         element_result == 42 && present_result == 7 && present_element == 7 &&
         fallback_calls == 4 && missing == null && global_value == null &&
         box.value == null && values[0] == null && present == 7 && values[1] == 7;
}
)";
  Isolate isolate(1);
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = test::Execute(**entry);
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsBoolean());
  EXPECT_TRUE(result.value.AsBoolean());
}

TEST(CompilerTest, ExecutesAsyncMainThroughRootFuture) {
  constexpr std::int64_t kAsyncResult = 3;
  constexpr std::string_view source = R"(
async func main(): Future<int> {
  return 3;
}
)";
  Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto main = (*entry)->FindFunction("main");
  ASSERT_TRUE(main);
  EXPECT_NE((*entry)->image().functions.values().subspan(*main).front().flags & runtime::kNativeAsyncFunction, 0U);
  auto root = test::StartFuture(**entry);
  EXPECT_EQ(root.state(), runtime::FutureState::Succeeded);
  EXPECT_FALSE(isolate.scheduler().PumpOne());
  const auto value = test::AwaitInteger(root, isolate);
  ASSERT_TRUE(value) << value.error();
  EXPECT_EQ(*value, kAsyncResult);
}

TEST(CompilerTest, SuspendsAsyncMainAtAwaitAndResumesThroughScheduler) {
  constexpr std::int64_t kAsyncResult = 4;
  constexpr std::string_view source = R"(
async func main(): Future<int> {
  await sleep(1);
  return 4;
}
)";
  Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  auto root = test::StartFuture(**entry);
  EXPECT_EQ(root.state(), runtime::FutureState::Pending);
  const auto value = test::AwaitInteger(root, isolate);
  ASSERT_TRUE(value) << value.error();
  EXPECT_EQ(*value, kAsyncResult);
}

TEST(CompilerTest, AsyncFailureRetainsDiagnosticAcrossRootFuture) {
  constexpr std::string_view source = R"(
async func main(): Future<void> {
  throw Error(message: "async failure");
}
)";
  Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  auto root = test::StartFuture(**entry);
  ASSERT_TRUE(isolate.scheduler().PumpUntil(root));
  ASSERT_EQ(root.state(), runtime::FutureState::Failed);
  const auto diagnostic = root.error_diagnostic();
  ASSERT_TRUE(diagnostic.has_value());
  const auto diagnostic_value = diagnostic.value_or(runtime::FutureDiagnostic{});
  EXPECT_EQ(diagnostic_value.message, "async failure");
  ASSERT_FALSE(diagnostic_value.stack_trace.empty());
  EXPECT_EQ(diagnostic_value.stack_trace.front().function, "main");
}

TEST(CompilerTest, SuspendedAsyncExecutionSurvivesHeapCollection) {
  constexpr std::string_view source = R"(
async func main(): Future<int> {
  await sleep(1);
  return 7;
}
)";
  Isolate isolate(1);
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  auto root = test::StartFuture(**entry);
  ASSERT_EQ(root.state(), runtime::FutureState::Pending);
  AllocateAndCollectTemporaryStrings(isolate);

  const auto value = test::AwaitInteger(root, isolate);
  ASSERT_TRUE(value) << value.error();
  EXPECT_EQ(*value, 7);
}

TEST(CompilerTest, FutureWaitAndRaceAreAvailableToVelaCode) {
  constexpr std::string_view source = R"(
async func main(): Future<int> {
  let values: Future<int>[] = [Future.value(2), Future.value(3)];
  let waited: int[] = await Future.wait(values);
  let winner: int = await Future.race(values);
  return waited[0] + waited[1] + winner;
}
)";
  Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  auto root = test::StartFuture(**entry);
  const auto value = test::AwaitInteger(root, isolate);
  ASSERT_TRUE(value) << value.error();
  EXPECT_EQ(*value, 7);
}

TEST(CompilerTest, FutureWaitCollectsPendingResultsInInputOrder) {
  constexpr std::string_view source = R"(
async func delayed(): Future<int> {
  await sleep(1);
  return 5;
}
async func main(): Future<int> {
  let values: Future<int>[] = [delayed(), Future.value(6)];
  let waited: int[] = await Future.wait(values);
  return waited[0] * 10 + waited[1];
}
)";
  Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  auto root = test::StartFuture(**entry);
  const auto value = test::AwaitInteger(root, isolate);
  ASSERT_TRUE(value) << value.error();
  EXPECT_EQ(*value, 56);
}

TEST(CompilerTest, FutureSyncInvokesComputationAndReturnsFuture) {
  constexpr std::string_view source = R"(
async func main(): Future<int> {
  return await Future.sync((): int => 8);
}
)";
  Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  auto root = test::StartFuture(**entry);
  const auto value = test::AwaitInteger(root, isolate);
  ASSERT_TRUE(value) << value.error();
  EXPECT_EQ(*value, 8);
}

TEST(CompilerTest, FutureDelayedAndMicrotaskInvokeCallbacks) {
  constexpr std::string_view source = R"(
async func main(): Future<int> {
  let delayed: int = await Future.delayed(1, (): int => 6);
  let microtask: int = await Future.microtask((): int => 7);
  return delayed + microtask;
}
)";
  Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  auto root = test::StartFuture(**entry);
  const auto value = test::AwaitInteger(root, isolate);
  ASSERT_TRUE(value) << value.error();
  EXPECT_EQ(*value, 13);
}

TEST(CompilerTest, CompilesNestedClassConstruction) {
  constexpr std::string_view source = R"(
class Theme {
  brightness: int;
}

class App {
  theme: Theme;
}

func main(): App {
  return App(theme: Theme(brightness: 1));
}
)";

  Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto baseline = isolate.heap().object_count();
  const auto result = test::Execute(**entry);
  ASSERT_EQ(result.status, ExecutionStatus::Returned);
  ASSERT_TRUE(result.value.IsObject());
  ASSERT_NE(result.value.AsObject(), nullptr);

  Value result_root = result.value;
  RootRange root(isolate.heap(), &result_root, 1);
  const Value* theme = vela::runtime::Heap::GetField(*result_root.AsObject(), 0);
  ASSERT_NE(theme, nullptr);
  ASSERT_TRUE(theme->IsObject());
  ASSERT_NE(theme->AsObject(), nullptr);
  const Value* brightness = vela::runtime::Heap::GetField(*theme->AsObject(), 0);
  ASSERT_NE(brightness, nullptr);
  ASSERT_TRUE(brightness->IsInteger());
  EXPECT_EQ(brightness->AsInteger(), 1);

  result_root = Value::Null();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), baseline);
}

TEST(CompilerTest, CompilesFunctionCall) {
  constexpr std::string_view source = R"(
func add(left: int, right: int): int {
  return left + right;
}

func main(): int {
  return add(20, 22);
}
)";

  Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = test::Execute(**entry);
  ASSERT_EQ(result.status, ExecutionStatus::Returned);
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), kExpectedSum);
}

TEST(CompilerTest, CompilesLoopControlFlow) {
  constexpr std::int64_t kExpectedLoopSum = 10;
  constexpr std::string_view source = R"(
func main(): int {
  let i: int = 0;
  let sum: int = 0;
  while (i != 5) {
    sum = sum + i;
    i = i + 1;
  }
  return sum;
}

)";
  Isolate isolate;
  const auto result = test::RunInteger(source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kExpectedLoopSum);
}

TEST(CompilerTest, ExecutesLanguageFeaturesAndNamedMethodArguments) {
  constexpr std::string_view source = R"(
class Counter {
  value: int;
  add(amount: int): int {
    this.value = this.value + amount;
    return this.value;
  }
  digits(tens: int, ones: int): int {
    return tens * 10 + ones;
  }
}

func main(): bool {
  let sum: int = 0;
  for (let i: int = 0; i < 5; i = i + 1) {
    if (i == 2) continue;
    if (i == 4) break;
    sum = sum + i;
  }
  let counter: Counter = Counter(value: sum);
  let text: string = "a" + "b";
  let values: int[] = [1, 2, 3];
  return counter.add(values[1]) == 6 && text == "ab" &&
         counter.digits(ones: 2, tens: 3) == 32;
}
)";
  Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = test::Execute(**entry);
  ASSERT_EQ(result.status, ExecutionStatus::Returned);
  ASSERT_TRUE(result.value.IsBoolean());
  EXPECT_TRUE(result.value.AsBoolean());
}

TEST(CompilerTest, ExecutesLoopWithWideJumpTarget) {
  constexpr std::int64_t kUpdateCount = 70;
  std::string source =
      "func main(): int { let value: int = 0; let counter: int = 0; while (counter < 1) {";
  for (std::int64_t index = 0; index < kUpdateCount; ++index) {
    source += "value = value + 1;";
  }
  source += "counter = counter + 1; } return value; }";
  Isolate isolate;
  const auto result = test::RunInteger(source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kUpdateCount);
}

TEST(CompilerTest, PreservesSymbolsAndShadowingAcrossTableGrowth) {
  constexpr std::int64_t kSymbolCount = 64;
  constexpr std::int64_t kLocalCount = 32;
  constexpr std::int64_t kExpectedResult = (kLocalCount * 2) - 1;
  std::string source;
  for (std::int64_t index = 0; index < kSymbolCount; ++index) {
    const std::string suffix = std::to_string(index);
    source += "enum SymbolEnum" + suffix + " { first, second }\n";
    source +=
        "class SymbolClass" + suffix + " { value: int; read(): int { return this.value; } }\n";
    source += "func symbol_function_" + suffix + "(value: int): int { return value; }\n";
  }
  source += "func main(): int { let sum: int = 0;\n";
  for (std::int64_t index = 0; index < kLocalCount; ++index) {
    const std::string suffix = std::to_string(index);
    source.append("let local_variable_with_a_long_name_")
        .append(suffix)
        .append(": int = ")
        .append(suffix)
        .append(";\n");
  }
  source += "{\n";
  for (std::int64_t index = 0; index < kLocalCount; ++index) {
    source += "let local_variable_with_a_long_name_" + std::to_string(index) +
              ": int = " + std::to_string(index + 1) + ";\n";
  }
  const std::string last_local =
      "local_variable_with_a_long_name_" + std::to_string(kLocalCount - 1);
  source += "sum = " + last_local + "; } sum = sum + " + last_local + ";\n";
  const std::string last_class = "SymbolClass" + std::to_string(kSymbolCount - 1);
  source += "let instance: " + last_class + " = " + last_class + "(value: sum);\n";
  source += "return symbol_function_0(instance.read()); }\n";

  Isolate isolate;
  const auto result = test::RunInteger(source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kExpectedResult);
}

TEST(CompilerTest, ExecutesTypedArraysAcrossCallsMethodsAndAssignments) {
  constexpr std::string_view source = R"(
enum Shade { light, dark }
class Item {
  value: int;
  read(): int { return this.value; }
}
class Store {
  items: Item[][];
  labels: string[];
  flags: bool[];
  shades: Shade[];
  read(): int { return this.items[0][0].read(); }
  replace(items: Item[]): Item[] {
    this.items[0] = items;
    return this.items[0];
  }
}
func identity(input: int[][]): int[][] { return input; }
func shades(): Shade[] { return [Shade.light, Shade.dark]; }
func flag(input: bool[]): bool { return input[0]; }
func main(): bool {
  let store: Store = Store(items: [[Item(value: 40)]], labels: ["old"],
                      flags: [false], shades: shades());
  let alias: bool[] = store.flags;
  alias[0] = true;
  store.labels[0] = "new";
  store.shades[0] = Shade.dark;
  store.items[0][0].value = 42;
  let replaced: Item[] = store.replace([Item(value: 42)]);
  let matrix: int[][] = identity([[], [1, 2]]);
  matrix[0] = [3];
  let row: int[] = matrix[1] = [4, 5];
  return store.read() == 42 && replaced[0].value == 42 && flag(store.flags) &&
         store.labels[0] == "new" && store.shades[0] == Shade.dark &&
         matrix[0][0] == 3 && row[1] == 5 &&
         [Item(value: 7)][0].read() == 7 && [true, false][0] &&
         ["a", "b"][1] == "b";
}
)";
  Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = test::Execute(**entry);
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsBoolean());
  EXPECT_TRUE(result.value.AsBoolean());
}

TEST(CompilerTest, ArrayFieldsAndNestedArraysRetainTheirObjects) {
  constexpr std::string_view source = R"(
class Item { value: int; }
class Store { items: Item[][]; flags: bool[]; labels: string[]; }
func main(): Store {
  return Store(items: [[Item(value: 42)]], flags: [true], labels: ["alive"]);
}
)";
  Isolate isolate(1);
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto baseline = isolate.heap().object_count();
  const auto result = test::Execute(**entry);
  ASSERT_TRUE(result.Succeeded());
  Value root = result.value;
  auto& heap = isolate.heap();
  RootRange roots(heap, &root, 1);
  heap.Collect();
  // Store、Item、两层 items 数组、flags 和 labels 共六个返回对象。
  EXPECT_EQ(heap.object_count(), baseline + 6U);
  const auto* items = runtime::Heap::GetField(*root.AsObject(), 0);
  ASSERT_NE(items, nullptr);
  const auto row = runtime::Heap::GetElement(*items->AsObject(), 0);
  ASSERT_TRUE(row.has_value());
  const auto item = runtime::Heap::GetElement(*row->AsObject(), 0);
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(runtime::Heap::GetField(*item->AsObject(), 0)->AsInteger(), kExpectedSum);
  root = Value::Null();
  heap.Collect();
  // 原生实例与 "alive" 常量仍存活，返回对象图已全部回收。
  EXPECT_EQ(heap.object_count(), baseline);
}

TEST(CompilerTest, ReportsArrayBoundsErrors) {
  constexpr auto kSources = std::to_array<std::string_view>({
      "func main(): int { let a: int[] = []; return a[0]; }",
      "func main(): int { let a: int[] = [1]; return a[-1]; }",
      "func main(): int { let a: int[] = [1]; a[1] = 2; return 0; }",
  });
  for (const auto source : kSources) {
    SCOPED_TRACE(source);
    Isolate isolate;
    const auto entry = test::CompileCleanSource(source, isolate);
    ASSERT_TRUE(entry) << entry.error();
    EXPECT_EQ(test::Execute(**entry).status, ExecutionStatus::IndexOutOfBounds);
  }
}

}  // namespace
}  // namespace vela::compiler
