#include <array>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/frontend/checker.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "vm/bytecode_image.h"
#include "vm/execution_status.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class DynamicArrayExecutionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(DynamicArrayExecutionTest, RunsTypedGrowthAndMutationThroughBytecodeImagesAndGc) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(GetParam(), isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ':' << diagnostic.location.column << ' '
                  << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto image = vm::SaveBytecode(compiled.program, isolate);
  ASSERT_TRUE(image);
  runtime::Isolate restored(1);
  const auto program = vm::LoadBytecode(*image, restored);
  ASSERT_TRUE(program) << program.error().message;
  restored.heap().FinishCollection();
  restored.heap().Collect();
  const auto baseline = restored.heap().object_count();
  vm::VirtualMachine machine(restored);
  const auto result = machine.Execute(program->FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 42);
  restored.heap().FinishCollection();
  restored.heap().Collect();
  EXPECT_EQ(restored.heap().object_count(), baseline);
}

INSTANTIATE_TEST_SUITE_P(ProductionPaths,
                         DynamicArrayExecutionTest,
                         testing::Values(
                             R"(func main(): int {
  const values: int[] = []; const alias = values;
  values.reserve(4096);
  for (let i = 0; i < 4096; i = i + 1) { values.push(i); }
  if (alias.length != 4096 || alias[4095] != 4095) { return 0; }
  values.splice(1, 0, 42);
  if (alias[1] != 42 || values.splice(1, 1)[0] != 42) { return 0; }
  const last = values.pop(); if (last == null || last != 4095) { return 0; }
  const capacity = values.capacity; values.clear();
  if (values.capacity != capacity || values.length != 0 || values.pop() != null) { return 0; }
  if (values.push(42) != 1) { return 0; }
  return alias[0];
})",
                             R"(func main(): int {
  const numbers: int?[] = []; numbers.push(null); numbers.unshift(42);
  const first = numbers.shift(); if (first == null) { return 0; }
  if (numbers.pop() != null || numbers.pop() != null) { return 0; }
  const flags: bool[] = []; flags.push(true); flags.unshift(false); flags.reverse();
  const flag = flags.pop(); if (flag == null || flag) { return 0; }
  const doubles: double[] = []; doubles.push(1.5); doubles.reserve(100); doubles.fill(2.5);
  const value = doubles.pop(); if (value == null || value != 2.5) { return 0; }
  return first;
})",
                             R"(class Item { param value: int; }
func main(): int {
  const items: Item[] = [];
  for (let i = 0; i < 1000; i = i + 1) { items.push(Item(value: i)); }
  const old = items[42]; const copy = items.slice(40, 44); items.clear();
  if (copy[2] != old) { return 0; }
  const nested: Item[][] = []; nested.push(copy); nested.reserve(100);
  const popped = nested.pop(); if (popped == null) { return 0; }
  return popped[2].value;
})",
                             R"(func main(): int {
  let values = [1]; const original = values;
  original.push((values = [40])[0]);
  original.splice(0, 0, (values = [42])[0]);
  if (original.length != 3 || values.length != 1 || original[2] != 40) { return 0; }
  return original.splice(0, 1)[0];
})",
                             R"(func main(): int {
  const values: ((int) => int)[] = [];
  for (let i = 0; i < 100; i = i + 1) {
    const offset = i; values.push((x: int): int => offset + x);
  }
  const operation = values.pop(); if (operation == null) { return 0; }
  const data: Expected<int, string>[] = []; data.push(ok(42));
  const result = data.pop(); if (result == null || operation(1) != 100) { return 0; }
  return result.value();
})",
                             R"(func main(): int {
  const words: string[] = [];
  for (let i = 0; i < 1000; i = i + 1) { words.push('word_${i}'); }
  words.reserve(2000); words.unshift('hello');
  if (words.shift() != 'hello' || words.pop() != 'word_999') { return 0; }
  const copy = words.slice(); copy.clear();
  if (words.length != 999 || words.slice(0, 2).join('/') != 'word_0/word_1') { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const values = [1, 2, 3];
  values.forEach(value => { values.reserve(100); values[0] = 42; });
  values.sort((left, right) => { values.reserve(200); return left - right; });
  return values[2];
})",
                             R"(func main(): int {
  const values: int[] = []; let caught = 0;
  try { print(values[-1]); } catch (error: Error) { caught = caught + 1; }
  try { print(values[0]); } catch (error: Error) { caught = caught + 1; }
  try { values.reserve(-1); } catch (error: Error) { caught = caught + 1; }
  try { values[0] = 1; } catch (error: Error) { caught = caught + 1; }
  try { print(values[4294967296]); } catch (error: Error) { caught = caught + 1; }
  if (caught == 5 && values.length == 0) { return 42; } return 0;
})"));

TEST(DynamicArrayTest, RejectsImplicitConversionsAndReadonlyLengthOrCapacityWrites) {
  const auto invalid = std::to_array<std::string_view>({
      "const values: int[] = []; values.push(1.0);",
      "const values: double[] = []; values.splice(0, 0, 1);",
      "const values: int[] = []; values.push(null);",
      "const values: int[] = []; const value: int = values.pop();",
      "const values: int[] = []; values.length = 42;",
      "const values: int[] = []; values.capacity = 42;",
      "const values: int[] = []; values.reserve(1.0);",
      "const values: int[] = []; const other: double[] = values;",
      "const values = [1]; values.insert(0, 2);",
      "const values = [1]; values.removeAt(0);",
      "const values = [1]; values.copy();",
      "const values = [1]; values.push(items: 2);",
      "const values = [1]; values.reduce(0, (a, b) => a + b);",
      "const values = [1]; values.reduce((a, b) => a + b);",
      "const values: int[] = []; values.reduce((a, b) => a + b);",
      "const values = [1]; values.map((a, b, c, d) => a);",
  });
  for (const auto body : invalid) {
    EXPECT_FALSE(
        frontend::CheckSource("func main(): void { " + std::string(body) + " }").Succeeded())
        << body;
  }
}

INSTANTIATE_TEST_SUITE_P(JavaScriptContracts,
                         DynamicArrayExecutionTest,
                         testing::Values(
                             R"(func main(): int {
  const a = [3];
  if (a.push() != 1 || a.unshift() != 1 || a.push(4, 5) != 3) { return 0; }
  if (a.unshift(1, 2) != 5 || a.join() != '1,2,3,4,5') { return 0; }
  if (a.shift() != 1 || a.pop() != 5) { return 0; }
  a.clear(); if (a.shift() != null || a.pop() != null) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const a = [0, 1, 2, 3]; const alias = a;
  const removed = a.splice(-3, 2, 8, 9, 10);
  if (removed.join() != '1,2' || alias.join() != '0,8,9,10,3') { return 0; }
  removed[0] = 42;
  if (a.splice().length != 0 || a.length != 5) { return 0; }
  if (a.splice(99, 5, 4).length != 0 || a.pop() != 4) { return 0; }
  if (a.splice(-2).join() != '10,3' || a.join() != '0,8,9') { return 0; }
  if (a.splice(-99, -2, 7).length != 0 || a.join() != '7,0,8,9') { return 0; }
  if (a.splice(0, 99).join() != '7,0,8,9' || a.length != 0) { return 0; }
  return removed[0];
})",
                             R"(func main(): int {
  const a: string?[] = ['a', null, 'c'];
  if (a.splice(1, 1, 'b', null).join() != '' || a.join() != 'a,b,,c') { return 0; }
  if (a.shift() != 'a' || a.pop() != 'c') { return 0; }
  const nested = [[1], [2]]; const removed = nested.splice(0, 1, [42]);
  if (removed[0][0] != 1 || nested[0][0] != 42) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  let values = [1]; const original = values; let order = 0;
  const next = (x: int): int => { order = order * 10 + x; values = [99]; return x; };
  original.push(next(2), next(3));
  original.splice(next(0), next(1), next(4), next(5));
  if (order != 230145 || original.join() != '4,5,2,3' || values[0] != 99) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const a = [1, 2, 3, 4];
  if (a.slice(-2).join() != '3,4' || a.slice(99).length != 0 ||
      a.slice(3, 1).length != 0 || a.slice(-99, 99).join() != '1,2,3,4') { return 0; }
  const filled = a.fill(8, -3, -1);
  if (filled.join() != '1,8,8,4') { return 0; }
  filled[0] = 5; if (a[0] != 5) { return 0; } filled[0] = 1;
  const reversed = a.fill(9, 3, 1).reverse();
  if (reversed.join() != '4,8,8,1') { return 0; }
  reversed[0] = 5; if (a[0] != 5) { return 0; } reversed[0] = 4;
  if (a.fill(7, end: 2).join() != '7,7,8,1') { return 0; }
  const copy = a.slice(); copy[0] = 42;
  if (a[0] != 7) { return 0; } return copy[0];
})",
                             R"(func main(): int {
  const a = [1, 2, 3]; let calls = 0;
  const sum = a.reduce((total, value, index, array) => {
    if (array[index] != a[index] || index != calls) { return -100; }
    calls = calls + 1; return total + value;
  }, 0);
  if (sum != 6 || calls != 3) { return 0; }
  if (a.reduce((s, v) => s + v.toString(), '') != '123') { return 0; }
  const empty: int[] = [];
  if (empty.reduce((s, v) => s + v, 42) != 42) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const a = [10, 20]; let calls = 0;
  a.forEach((value, index, array) => { calls = calls + 1; return array[index] + value; });
  const b = a.map((value, index, array) => value + index + array.length);
  if (calls != 2 || b.join() != '12,23') { return 0; }
  if (!a.some((value, index) => index == 1) || !a.every(() => true) ||
      a.findIndex((value, index, array) => array[index] == 20) != 1) { return 0; }
  return a.map(() => 42)[0];
})",
                             R"(class Item { param key: int; param id: int; }
func main(): int {
  const a = [Item(key: 1, id: 1), Item(key: 0, id: 2), Item(key: 1, id: 3), Item(key: 0, id: 4)];
  if (a.sort((x, y) => x.key - y.key).map(x => x.id).join() != '2,4,1,3') { return 0; }
  if (a.sort(() => 0).map(x => x.id).join() != '2,4,1,3') { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const numbers = [10, 2, 1];
  const sorted = numbers.sort();
  if (numbers.join() != '1,10,2') { return 0; }
  sorted[0] = 42; if (numbers[0] != 42) { return 0; }
  const text = ['\uE000', '😀', 'a']; text.sort();
  if (text[0] != 'a' || text[1] != '😀') { return 0; }
  const doubles = [2.5, 1.5, 3.5];
  if (doubles.sort((a, b) => a - b).join() != '1.5,2.5,3.5') { return 0; }
  if (doubles.sort(() => Math.pow(-1.0, 0.5)).join() != '1.5,2.5,3.5') { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const a = [3, 1, 2];
  a.sort((x, y) => { a[0] = 99; return x - y; });
  if (a.join() != '1,2,3') { return 0; }
  const empty: int[] = []; empty.sort().push(42); if (empty[0] != 42) { return 0; }
  return 42;
})",
                             R"(func main(): int {
  const a: double?[] = [-0.0, null, 0.000001, 0.0000001, 1e20, 1e21];
  if (a.join() != '0,,0.000001,1e-7,100000000000000000000,1e+21') { return 0; }
  const b = [Math.pow(-1.0, 0.5), Math.exp(1000.0), -Math.exp(1000.0)];
  if (b.join() != 'NaN,Infinity,-Infinity') { return 0; }
  return 42;
})"));

class ArrayCallbackMutationTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ArrayCallbackMutationTest, DetectsMutationBeforeUsingCallbackResultsAndVmRemainsReusable) {
  runtime::Isolate isolate(1);
  const auto source = "func main(): void { const values = [3, 1, 2]; " + std::string(GetParam()) +
                      " } func answer(): int { return 42; }";
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  vm::VirtualMachine machine(isolate);
  const auto result = machine.Execute(compiled.program.FindFunction("main"));
  EXPECT_EQ(result.status, vm::ExecutionStatus::ConcurrentModification) << result.message;
  const auto answer = machine.Execute(compiled.program.FindFunction("answer"));
  ASSERT_TRUE(answer.Succeeded());
  EXPECT_EQ(answer.value.AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(
    Callbacks,
    ArrayCallbackMutationTest,
    testing::Values("values.forEach(value => { values.push(value); });",
                    "values.map(value => { values.clear(); return value; });",
                    "values.some(value => { values.pop(); return true; });",
                    "values.every(value => { values.unshift(value); return false; });",
                    "values.findIndex(value => { values.shift(); return true; });",
                    "values.reduce((total, value) => { values.pop(); return total + value; }, 0);",
                    "values.sort((left, right) => { values.clear(); return left - right; });",
                    "values.map(value => { values.push(value); values.pop(); return value; });"));

}  // namespace
}  // namespace vela::compiler
