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
  values.insert(1, 42);
  if (alias[1] != 42 || values.removeAt(1) != 42) { return 0; }
  const last = values.pop(); if (last == null || last != 4095) { return 0; }
  const capacity = values.capacity; values.clear();
  if (values.capacity != capacity || values.length != 0 || values.pop() != null) { return 0; }
  if (values.push(42) != 1) { return 0; }
  return alias[0];
})",
                             R"(func main(): int {
  const numbers: int?[] = []; numbers.push(null); numbers.insert(0, 42);
  const first = numbers.removeAt(0); if (first == null) { return 0; }
  if (numbers.pop() != null || numbers.pop() != null) { return 0; }
  const flags: bool[] = []; flags.push(true); flags.insert(0, false); flags.reverse();
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
  original.insert(value: (values = [42])[0], index: 0);
  if (original.length != 3 || values.length != 1 || original[2] != 40) { return 0; }
  return original.removeAt(0);
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
  words.reserve(2000); words.insert(0, 'hello');
  if (words.removeAt(0) != 'hello' || words.pop() != 'word_999') { return 0; }
  const copy = words.copy(); copy.clear();
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
  try { values.insert(-1, 1); } catch (error: Error) { caught = caught + 1; }
  try { values.removeAt(0); } catch (error: Error) { caught = caught + 1; }
  try { values.reserve(-1); } catch (error: Error) { caught = caught + 1; }
  try { values[0] = 1; } catch (error: Error) { caught = caught + 1; }
  try { values.insert(4294967296, 1); } catch (error: Error) { caught = caught + 1; }
  if (caught == 5 && values.length == 0) { return 42; } return 0;
})"));

TEST(DynamicArrayTest, RejectsImplicitConversionsAndReadonlyLengthOrCapacityWrites) {
  const auto invalid = std::to_array<std::string_view>({
      "const values: int[] = []; values.push(1.0);",
      "const values: double[] = []; values.insert(0, 1);",
      "const values: int[] = []; values.push(null);",
      "const values: int[] = []; const value: int = values.pop();",
      "const values: int[] = []; values.length = 42;",
      "const values: int[] = []; values.capacity = 42;",
      "const values: int[] = []; values.reserve(1.0);",
      "const values: int[] = []; const other: double[] = values;",
  });
  for (const auto body : invalid) {
    EXPECT_FALSE(
        frontend::CheckSource("func main(): void { " + std::string(body) + " }").Succeeded())
        << body;
  }
}

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
                    "values.every(value => { values.insert(0, value); return false; });",
                    "values.findIndex(value => { values.removeAt(0); return true; });",
                    "values.reduce(0, (total, value) => { values.pop(); return total + value; });",
                    "values.sort((left, right) => { values.clear(); return left - right; });",
                    "values.map(value => { values.push(value); values.pop(); return value; });"));

}  // namespace
}  // namespace vela::compiler
