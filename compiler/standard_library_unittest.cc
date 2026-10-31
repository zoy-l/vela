#include <array>
#include <chrono>
#include <ratio>
#include <sstream>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/frontend/checker_test_support.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/program_test_support.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_context.h"
#include "runtime/native_function.h"
#include "runtime/value.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class StandardLibraryExecutionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(StandardLibraryExecutionTest, UsesRealModuleCompilerAndVmPaths) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(GetParam(), isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

INSTANTIATE_TEST_SUITE_P(Libraries,
                         StandardLibraryExecutionTest,
                         testing::Values(
                             R"(
func main(): int {
  if (Math.sqrt(9.0) == 3.0 && Math.pow(2.0, 3.0) == 8.0 && Math.sin(0.0) == 0.0 &&
      Math.cos(0.0) == 1.0 && Math.floor(2.9) == 2.0 && Math.ceil(2.1) == 3.0 &&
      Math.round(-1.5) == -2.0 && Math.trunc(-1.9) == -1.0 && Math.abs(-2.0) == 2.0 &&
      Math.min(2.0, 3.0) == 2.0 && Math.max(2.0, 3.0) == 3.0 &&
      Math.clamp(10.0, 0.0, 5.0) == 5.0 && Math.isFinite(Math.PI) && Math.E > 2.0) {
    return 42;
  }
  return 0;
})",
                             R"(
func main(): int {
  let caught = 0;
  try { Math.sqrt(-1.0); } catch (error: Error) { caught = caught + 1; }
  try { Math.log(0.0); } catch (error: Error) { caught = caught + 1; }
  try { Math.clamp(0.0, 2.0, 1.0); } catch (error: Error) { caught = caught + 1; }
  if (caught == 3 && Math.isNaN(Math.pow(-1.0, 0.5)) && !Math.isFinite(Math.exp(1000.0))) {
    return 42;
  }
  return 0;
})",
                             R"(
func main(): int {
  Random.seed(123);
  const first = Random.nextInt(1000);
  Random.seed(123);
  if (first != Random.nextInt(1000)) { return 0; }
  for (let index = 0; index < 500; index = index + 1) {
    const value = Random.nextInt(7);
    const fraction = Math.random();
    if (value < 0 || value >= 7 || fraction < 0.0 || fraction >= 1.0) { return 0; }
  }
  try { Random.nextInt(0); } catch (error: Error) { return 42; }
  return 0;
})",
                             R"(
func main(): int {
  const start = performance.now();
  const finish = performance.now();
  if (finish >= start && Date.now() > 0) { return 42; }
  return 0;
})",
                             R"(
func main(): int {
  const values: int[] = [1, 2, 3];
  let sum = 0;
  values.forEach((value: int): void => { sum = sum + value; });
  if (sum != 6 || !values.some((value: int): bool => value == 2) ||
      !values.every((value: int): bool => value > 0) ||
      values.findIndex((value: int): bool => value == 3) != 2) { return 0; }
  const mapped = values.map(value => value * 2);
  if (values[0] != 1) { return 0; }
  return mapped.reduce((sum: int, value: int): int => sum + value, 30);
})",
                             R"(
func main(): int {
  const a: Set<int> = Set<int>(); const b: Set<int> = Set<int>();
  a.add(1); a.add(2); b.add(2); b.add(3);
  const combined = a.union(b);
  const common = a.intersection(b);
  const different = a.difference(b);
  const order = combined.values();
  if (combined.size == 3 && common.size == 1 && common.has(2) &&
      different.has(1) && !different.has(2) && order[0] == 1 && order[2] == 3) { return 42; }
  return 0;
})",
                             R"(
func main(): int {
  const values: int[] = [1, 2, 3];
  const copy = values.slice(); copy.reverse();
  const slice = copy.slice(0, 2); copy.fill(0);
  if (values[0] == 1 && copy[0] == 0 && slice[0] == 3 && slice[1] == 2 &&
      copy.slice(1, 1).length == 0) { return 42; }
  return 0;
})",
                             R"(
class Item { value: int; }
func main(): int {
  const values: Item[] = [Item(value: 40), Item(value: 2)];
  const copy = values.slice(); copy.reverse();
  values.fill(Item(value: 0));
  const strings: string[] = ["a", "b"]; const texts = strings.slice(); strings.fill("changed");
  const nullable: int?[] = [null, 42]; const other = nullable.slice(); other.reverse();
  if (texts[0] == "a" && other[0] == 42 && other[1] == null) {
    return copy[0].value + copy[1].value;
  }
  return 0;
})",
                             R"(
func main(): int {
  const values = [1, 2];
  if (values.slice(-1, 1).length == 0 && values.slice(2, 1).length == 0 &&
      values.slice(0, 3).length == 2) { return 42; } return 0;
})",
                             R"(
func main(): int {
  const values: int[] = [3, 1, 2, 2];
  values.sort((left: int, right: int): int => left - right);
  const empty: int[] = [];
  empty.sort((left: int, right: int): int => left - right);
  if (values[0] == 1 && values[1] == 2 && values[2] == 2 && values[3] == 3) { return 42; }
  return 0;
})"));

TEST(StandardOutputTest, RedirectsPrint) {
  runtime::Isolate isolate(1);
  std::ostringstream output;
  isolate.SetOutput(output);
  const auto result = test::RunInteger(R"(
func main(): int {
  print("prompt>");
  print('你好');
  return 42;
})",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
  EXPECT_EQ(output.str(), "prompt>\n你好\n");
}

TEST(StandardLibraryTest, DoesNotRegisterConsoleBindings) {
  runtime::Isolate isolate;
  EXPECT_EQ(isolate.FindNative("Console.readLine"), nullptr);
  EXPECT_EQ(isolate.FindNative("Console.write"), nullptr);
  EXPECT_EQ(isolate.FindNative("Console.flush"), nullptr);
}

TEST(StandardClockTest, DateNowReturnsIntegerUnixMilliseconds) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource("func main(): int { return Date.now(); }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  const auto after = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  ASSERT_TRUE(result.Succeeded() && result.value.IsInteger());
  EXPECT_GE(result.value.AsInteger(), before);
  EXPECT_LE(result.value.AsInteger(), after);
}

TEST(StandardClockTest, PerformanceNowReturnsFractionalMonotonicMilliseconds) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource("func main(): double { return performance.now(); }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto before =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  const auto after =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
  ASSERT_TRUE(result.Succeeded() && result.value.IsDouble());
  EXPECT_GE(result.value.AsDouble(), before);
  EXPECT_LE(result.value.AsDouble(), after);
}

TEST(StandardLibraryTest, MapAllocatesOnlyOneResultArray) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(R"(func main(): int {
    const values = [1, 2, 3]; const mapped = values.map(value => value + 1);
    return mapped[0];
  })",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  auto& heap = isolate.heap();
  const auto before = heap.object_count();
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  // 原始数组、回调闭包和最终结果数组；循环不创建中间数组。
  EXPECT_EQ(heap.object_count(), before + 3);
}

TEST(StandardLibraryTest, RejectsImplicitConversionsAndRemovedModuleScheme) {
  EXPECT_FALSE(
      frontend::CheckTestSource("func main(): double { return Math.sqrt(4); }").Succeeded());
  EXPECT_FALSE(
      frontend::CheckTestSource("func main(): void { const values = [1]; values.fill(1.0); }")
          .Succeeded());
  EXPECT_FALSE(frontend::CheckTestSource("import \"std:missing\";").Succeeded());
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import \"std:math\";"},
      {.path = "std:math", .text = "export func sqrt(value: double): double { return 0.0; }"},
  });
  EXPECT_FALSE(frontend::CheckTestModules(sources, "main.vela").Succeeded());
}

class ArrayAlgorithmExecutionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ArrayAlgorithmExecutionTest, PreservesTypedCallbacksEvaluationOrderAndGcRoots) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(GetParam(), isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

INSTANTIATE_TEST_SUITE_P(TraversalSortAndReduction,
                         ArrayAlgorithmExecutionTest,
                         testing::Values(
                             R"(func main(): int {
  const original = [1, 2, 3];
  const mapped = original.map(value => value.toString());
  mapped[0] = 'changed';
  if (original[0] == 1 && mapped.join(',') == 'changed,2,3') { return 42; }
  return 0;
})",
                             R"(func main(): int {
  const numbers = [1, 2].map(value => value.toDouble());
  const optional = [1, 2].map((value): string? => { if (value == 1) { return null; } return 'yes'; });
  if (numbers[1] == 2.0 && optional.join(',') == ',yes') { return 42; }
  return 0;
})",
                             R"(func main(): int {
  const callbacks = [1, 2].map(value => () => value + 40);
  return callbacks[1]();
})",
                             R"(func main(): int {
  const nested = [1, 2].map(value => [value.toString()]).map(row => row.map(text => text + '!'));
  if (nested[1][0] == '2!') { return 42; } return 0;
})",
                             R"(func main(): int {
  let values = [40, 2]; const original = values; let order = 0;
  const get = () => { order = order * 10 + 1; return values; };
  const operation = () => { order = order * 10 + 2; values = [100]; return (value: int) => value + 1; };
  const mapped = get().map(operation());
  if (order == 12 && original[0] == 40 && mapped[0] == 41) { return mapped[0] + 1; }
  return 0;
})",
                             R"(func main(): int {
  let caught = 0;
  try { [1, 2].map((value): int => { throw Error(message: 'stop'); }); }
  catch (error: Error) { caught = 42; }
  return caught;
})",
                             R"(func main(): int {
  const empty: int[] = []; let calls = 0;
  const predicate = (value: int): bool => { calls = calls + 1; return true; };
  if (empty.some(predicate) || !empty.every(predicate) || empty.findIndex(predicate) != -1) { return 0; }
  empty.forEach((value: int): void => { calls = calls + 1; });
  const mapped = empty.map(value => { calls = calls + 1; return value; });
  if (mapped.length != 0) { return 0; }
  if (calls == 0) { return empty.reduce((a: int, b: int): int => a + b, 42); }
  return 0;
})",
                             R"(func main(): int {
  const values = [1, 2, 3]; let calls = 0;
  if (!values.some((value: int): bool => { calls = calls + 1; return value == 2; }) || calls != 2) { return 0; }
  if (values.every((value: int): bool => { calls = calls + 1; return value < 2; }) || calls != 4) { return 0; }
  if (values.findIndex((value: int): bool => { calls = calls + 1; return value == 1; }) != 0 || calls != 5) { return 0; }
  return 42;
})",
                             R"(class Item { value: int; }
func main(): int {
  const values = [Item(value: 40), Item(value: 1)];
  const mapped = values.map(item => Item(value: item.value + 1));
  const text = mapped.reduce((sum: string, item: Item): string => sum + item.value.toString(), '');
  if (text == '412' && values[0].value == 40) { return 42; } return 0;
})",
                             R"(func main(): int {
  let values = [40, 2]; const original = values; let order = 0;
  const get = (): int[] => { order = order * 10 + 1; return values; };
  const initial = (): int => { order = order * 10 + 2; values = [100]; return 0; };
  const callback = (): (int, int) => int => { order = order * 10 + 3; return (a: int, b: int): int => a + b; };
  const result = get().reduce(initialValue: initial(), callbackFn: callback());
  if (order == 123 && original[0] == 40 && values[0] == 100) { return result; } return 0;
})",
                             R"(func main(): int {
  const values = [1, 2];
  return values.reduce((sum: int, value: int): int => {
    if (value == 1) { values[1] = 41; }
    return sum + value;
  }, 0);
})",
                             R"(func main(): int {
  const values: int?[] = [null, 42];
  return values.reduce((sum: int, value: int?): int => {
    if (value == null) { return sum; } return sum + value;
  }, 0);
})",
                             R"(class Item { value: int; }
func main(): int {
  const values = [Item(value: 2), Item(value: 40), Item(value: 1), Item(value: 2)];
  values.sort((a: Item, b: Item): int => b.value - a.value);
  const numbers = [3.0, -1.0, 2.0];
  numbers.sort((a: double, b: double): int => { if (a < b) { return -1; } if (a > b) { return 1; } return 0; });
  if (values[0].value == 40 && values[3].value == 1 && numbers[0] == -1.0 && numbers[2] == 3.0) { return 42; }
  return 0;
})",
                             R"(func main(): int {
  const values = [2, 1]; let caught = 0;
  try { values.sort((a: int, b: int): int => { throw Error(message: 'sort'); }); } catch (error: Error) { caught = caught + 1; }
  try { values.forEach((value: int): void => { throw Error(message: 'each'); }); } catch (error: Error) { caught = caught + 1; }
  try { values.reduce((a: int, b: int): int => { throw Error(message: 'reduce'); }, 0); } catch (error: Error) { caught = caught + 1; }
  if (caught == 3) { return 42; } return 0;
})",
                             R"(func main(): int {
  const a = Set<string>(); const b = Set<string>();
  a.add('left'); a.add('same'); a.delete('left'); a.add('left');
  b.add('same'); b.add('right');
  const combined = a.union(b); const common = a.intersection(b); const different = a.difference(b);
  const order = combined.values(); a.clear(); b.clear();
  if (order[0] == 'same' && order[1] == 'left' && order[2] == 'right' &&
      common.has('same') && different.has('left') && combined.size == 3) { return 42; }
  return 0;
})"));

class InvalidStandardApiTest : public testing::TestWithParam<std::string_view> {};
TEST_P(InvalidStandardApiTest, RejectsInvalidContracts) {
  EXPECT_FALSE(frontend::CheckTestSource(GetParam()).Succeeded());
}
INSTANTIATE_TEST_SUITE_P(
    StrictSignatures,
    InvalidStandardApiTest,
    testing::Values(
        "func main(): void { [1].sort((a: int, b: int): bool => true); }",
        "func main(): void { [1].some((a: double): bool => true); }",
        "func main(): void { [1].map((a: int): void => {}); }",
        "func main(): void { [1].reduce((a: int, b: int): int => a + b, 0.0); }",
        "func main(): void { Set<int>().union(Set<double>()); }",
        "func main(): void { Math.PI = 0.0; }",
        "func main(): void { Math(); }",
        "func main(): void { Console(); }",
        "func main(): void { Console.readLine(); }",
        "func main(): void { Console.write('removed'); }",
        "func main(): void { Console.flush(); }",
        "func main(): int { return Time.monotonicMicros(); }",
        "func main(): int { return Time.unixMicros(); }",
        "func main(): double { return Random.nextDouble(); }",
        "func main(): double { return Math.pi; }",
        "func main(): bool { return [1].any(value => true); }",
        "func main(): bool { return 'x'.contains('x'); }",
        "func main(): void { [1].transform(value => value + 1); }",
        "func main(): bool { return Set<int>().remove(1); }",
        "func main(): int { return Map<int, int>().length; }",
        "func main(): void { const x: int[] = [1].map(x => x.toString()); }",
        "func main(): void { [1].map((x: string) => x); }",
        "func broken(value: int): Missing { return value; } func main(): void { [1].map(broken); }",
        "import 'std:collections'; func main(): void {}",
        "import 'std:math'; func main(): void {}"));

TEST(StandardLibraryTest, DoesNotAddStandardDeclarationsForNamesInsideStringsOrComments) {
  const auto source = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text =
              "// Math Error\nfunc main(): string { return 'Console Random performance Date'; }",
      },
  });
  const auto loaded = frontend::LoadSources(source, "main.vela");
  ASSERT_TRUE(loaded.diagnostics.empty());
  EXPECT_EQ(loaded.program.sources.size(), 1U);
  EXPECT_TRUE(loaded.program.classes.empty());
}

double HostClock(runtime::NativeContext& /*unused*/) noexcept {
  constexpr double kHostClockValue = 42.0;
  return kHostClockValue;
}
TEST(StandardLibraryTest, ValidatesStaticExternalBindingsAndUsesExplicitHostRegistration) {
  runtime::Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<HostClock>("performance.now")));
  const auto compiled = CompileSource("func main(): double { return performance.now(); }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_DOUBLE_EQ(result.value.AsDouble(), 42.0);
  EXPECT_FALSE(CompileSource("class Host { static external func missing(): int = 'missing'; } func "
                             "main(): int { return Host.missing(); }",
                             isolate)
                   .Succeeded());
  EXPECT_FALSE(CompileSource("class Host { static external func clock(value: int): int = "
                             "'performance.now'; } func main(): int { return Host.clock(1); }",
                             isolate)
                   .Succeeded());
}

}  // namespace
}  // namespace vela::compiler
