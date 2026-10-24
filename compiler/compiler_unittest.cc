#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/value.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

constexpr std::int64_t kExpectedSum = 42;

using vela::runtime::Isolate;
using vela::runtime::RootRange;
using vela::runtime::Value;
using vela::vm::ExecutionStatus;
using vela::vm::VirtualMachine;

TEST(CompilerTest, CompilesQualifiedEnumMemberWithoutExpectedType) {
  constexpr std::string_view source = R"(
enum Shade { light, dark }
func main(): Shade {
  const value = Shade.dark;
  return value;
}
)";
  Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto result = VirtualMachine(isolate).Execute(function->View());
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), 1);
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
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto result = VirtualMachine(isolate).Execute(function->View());
  ASSERT_EQ(result.status, ExecutionStatus::Returned);
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), kExpectedSum);
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
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto result = VirtualMachine(isolate).Execute(function->View());
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
  EXPECT_EQ(isolate.heap().object_count(), 0U);
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
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto entry = static_cast<std::uint32_t>(function - compiled.program.functions.data());
  const auto result = VirtualMachine(isolate).Execute(compiled.program.View(), entry);
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
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto result = VirtualMachine(isolate).Execute(function->View());
  ASSERT_EQ(result.status, ExecutionStatus::Returned);
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), kExpectedLoopSum);
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
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto result = VirtualMachine(isolate).Execute(
      compiled.program.View(),
      static_cast<std::uint32_t>(function - compiled.program.functions.data()));
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
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto result = VirtualMachine(isolate).Execute(function->View());
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), kUpdateCount);
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
  const auto compiled = CompileSource(source, isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto* entry = compiled.program.FindFunction("main");
  ASSERT_NE(entry, nullptr);
  const auto entry_index = static_cast<std::uint32_t>(entry - compiled.program.functions.data());
  const auto result = VirtualMachine(isolate).Execute(compiled.program.View(), entry_index);
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), kExpectedResult);
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
  const auto compiled = CompileSource(source, isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ": " << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto* entry = compiled.program.FindFunction("main");
  ASSERT_NE(entry, nullptr);
  const auto index = static_cast<std::uint32_t>(entry - compiled.program.functions.data());
  const auto result = VirtualMachine(isolate).Execute(compiled.program.View(), index);
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
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* entry = compiled.program.FindFunction("main");
  ASSERT_NE(entry, nullptr);
  const auto result = VirtualMachine(isolate).Execute(entry->View());
  ASSERT_TRUE(result.Succeeded());
  Value root = result.value;
  auto& heap = isolate.heap();
  RootRange roots(heap, &root, 1);
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 7U);
  const auto* items = runtime::Heap::GetField(*root.AsObject(), 0);
  ASSERT_NE(items, nullptr);
  const auto row = runtime::Heap::GetElement(*items->AsObject(), 0);
  ASSERT_TRUE(row.has_value());
  const auto item = runtime::Heap::GetElement(*row->AsObject(), 0);
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(runtime::Heap::GetField(*item->AsObject(), 0)->AsInteger(), kExpectedSum);
  root = Value::Null();
  heap.Collect();
  // 编译结果仍持有 "alive" 常量，其余对象已回收。
  EXPECT_EQ(heap.object_count(), 1U);
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
    const auto compiled = CompileSource(source, isolate);
    ASSERT_TRUE(compiled.Succeeded());
    const auto* entry = compiled.program.FindFunction("main");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(VirtualMachine(isolate).Execute(entry->View()).status,
              ExecutionStatus::IndexOutOfBounds);
  }
}

}  // namespace
}  // namespace vela::compiler
