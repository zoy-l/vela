#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/frontend/checker.h"
#include "compiler/frontend/source_loader.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class StandardLibraryExecutionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(StandardLibraryExecutionTest, UsesRealModuleCompilerAndVmPaths) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(GetParam(), isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.source_path << ':' << diagnostic.location.line << ' '
                  << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 42);
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
      Math.clamp(10.0, 0.0, 5.0) == 5.0 && Math.isFinite(Math.pi) && Math.e > 2.0) {
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
    const fraction = Random.nextDouble();
    if (value < 0 || value >= 7 || fraction < 0.0 || fraction >= 1.0) { return 0; }
  }
  try { Random.nextInt(0); } catch (error: Error) { return 42; }
  return 0;
})",
                             R"(
func main(): int {
  const start = Time.monotonicMicros();
  const finish = Time.monotonicMicros();
  if (finish >= start && Time.unixMicros() > 0) { return 42; }
  return 0;
})",
                             R"(
func main(): int {
  const values: int[] = [1, 2, 3];
  let sum = 0;
  values.forEach((value: int): void => { sum = sum + value; });
  if (sum != 6 || !values.any((value: int): bool => value == 2) ||
      !values.every((value: int): bool => value > 0) ||
      values.findIndex((value: int): bool => value == 3) != 2) { return 0; }
  values.transform((value: int): int => value * 2);
  return values.reduce(30, (sum: int, value: int): int => sum + value);
})",
                             R"(
func main(): int {
  const a: Set<int> = Set<int>(); const b: Set<int> = Set<int>();
  a.add(1); a.add(2); b.add(2); b.add(3);
  const combined = a.union(b);
  const common = a.intersection(b);
  const different = a.difference(b);
  const order = combined.values();
  if (combined.length == 3 && common.length == 1 && common.has(2) &&
      different.has(1) && !different.has(2) && order[0] == 1 && order[2] == 3) { return 42; }
  return 0;
})",
                             R"(
func main(): int {
  const values: int[] = [1, 2, 3];
  const copy = values.copy(); copy.reverse();
  const slice = copy.slice(0, 2); copy.fill(0);
  if (values[0] == 1 && copy[0] == 0 && slice[0] == 3 && slice[1] == 2 &&
      copy.slice(1, 1).length == 0) { return 42; }
  return 0;
})",
                             R"(
class Item { param value: int; }
func main(): int {
  const values: Item[] = [Item(value: 40), Item(value: 2)];
  const copy = values.copy(); copy.reverse();
  values.fill(Item(value: 0));
  const strings: string[] = ["a", "b"]; const texts = strings.copy(); strings.fill("changed");
  const nullable: int?[] = [null, 42]; const other = nullable.copy(); other.reverse();
  if (texts[0] == "a" && other[0] == 42 && other[1] == null) {
    return copy[0].value + copy[1].value;
  }
  return 0;
})",
                             R"(
func main(): int {
  const values = [1, 2]; let count = 0;
  try { values.slice(-1, 1); } catch (error: Error) { count = count + 1; }
  try { values.slice(2, 1); } catch (error: Error) { count = count + 1; }
  try { values.slice(0, 3); } catch (error: Error) { count = count + 1; }
  if (count == 3) { return 42; } return 0;
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

TEST(StandardConsoleTest, ReadsUtf8CrLfEmptyLinesAndEofAndRedirectsPrint) {
  runtime::Isolate isolate(1);
  std::istringstream input("你好\r\n\nlast");
  std::ostringstream output;
  isolate.SetConsole(input, output);
  const auto compiled = CompileSource(R"(
func main(): int {
  Console.write("prompt> "); Console.flush();
  const first = Console.readLine(); const empty = Console.readLine(); const last = Console.readLine();
  if (first != null && empty == "" && last == "last" && Console.readLine() == null) {
    print(first); return 42;
  }
  return 0;
})",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
  EXPECT_EQ(output.str(), "prompt> 你好\n");
}

TEST(StandardConsoleTest, InvalidUtf8InputBecomesCatchableError) {
  runtime::Isolate isolate;
  constexpr unsigned char kInvalidByte = 0xff;
  std::istringstream input(std::string(1, static_cast<char>(kInvalidByte)));
  std::ostringstream output;
  isolate.SetConsole(input, output);
  const auto compiled = CompileSource(R"(
func main(): string {
  try { Console.readLine(); } catch (error: Error) { return error.message; }
  return "not caught";
})",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsString()->value(), "console input is not valid UTF-8");
}

TEST(StandardLibraryTest, RejectsImplicitConversionsAndRemovedModuleScheme) {
  EXPECT_FALSE(frontend::CheckSource("func main(): double { return Math.sqrt(4); }").Succeeded());
  EXPECT_FALSE(frontend::CheckSource("func main(): void { const values = [1]; values.fill(1.0); }")
                   .Succeeded());
  EXPECT_FALSE(frontend::CheckSource("import \"std:missing\";").Succeeded());
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import \"std:math\";"},
      {.path = "std:math", .text = "export func sqrt(value: double): double { return 0.0; }"},
  });
  EXPECT_FALSE(frontend::CheckModules(sources, "main.vela").Succeeded());
}

class ArrayAlgorithmExecutionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ArrayAlgorithmExecutionTest, PreservesTypedCallbacksEvaluationOrderAndGcRoots) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(GetParam(), isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(TraversalSortAndReduction,
                         ArrayAlgorithmExecutionTest,
                         testing::Values(
                             R"(func main(): int {
  const empty: int[] = []; let calls = 0;
  const predicate = (value: int): bool => { calls = calls + 1; return true; };
  if (empty.any(predicate) || !empty.every(predicate) || empty.findIndex(predicate) != -1) { return 0; }
  empty.forEach((value: int): void => { calls = calls + 1; });
  empty.transform((value: int): int => { calls = calls + 1; return value; });
  if (calls == 0) { return empty.reduce(42, (a: int, b: int): int => a + b); }
  return 0;
})",
                             R"(func main(): int {
  const values = [1, 2, 3]; let calls = 0;
  if (!values.any((value: int): bool => { calls = calls + 1; return value == 2; }) || calls != 2) { return 0; }
  if (values.every((value: int): bool => { calls = calls + 1; return value < 2; }) || calls != 4) { return 0; }
  if (values.findIndex((value: int): bool => { calls = calls + 1; return value == 1; }) != 0 || calls != 5) { return 0; }
  return 42;
})",
                             R"(class Item { param value: int; }
func main(): int {
  const values = [Item(value: 40), Item(value: 1)];
  values.transform((item: Item): Item => Item(value: item.value + 1));
  const text = values.reduce('', (sum: string, item: Item): string => sum + item.value.toString());
  if (text == '412') { return 42; } return 0;
})",
                             R"(func main(): int {
  let values = [40, 2]; const original = values; let order = 0;
  const get = (): int[] => { order = order * 10 + 1; return values; };
  const initial = (): int => { order = order * 10 + 2; values = [100]; return 0; };
  const callback = (): (int, int) => int => { order = order * 10 + 3; return (a: int, b: int): int => a + b; };
  const result = get().reduce(initial: initial(), operation: callback());
  if (order == 123 && original[0] == 40 && values[0] == 100) { return result; } return 0;
})",
                             R"(func main(): int {
  const values = [1, 2];
  return values.reduce(0, (sum: int, value: int): int => {
    if (value == 1) { values[1] = 41; }
    return sum + value;
  });
})",
                             R"(func main(): int {
  const values: int?[] = [null, 42];
  return values.reduce(0, (sum: int, value: int?): int => {
    if (value == null) { return sum; } return sum + value;
  });
})",
                             R"(class Item { param value: int; }
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
  try { values.reduce(0, (a: int, b: int): int => { throw Error(message: 'reduce'); }); } catch (error: Error) { caught = caught + 1; }
  if (caught == 3) { return 42; } return 0;
})",
                             R"(func main(): int {
  const a = Set<string>(); const b = Set<string>();
  a.add('left'); a.add('same'); a.remove('left'); a.add('left');
  b.add('same'); b.add('right');
  const combined = a.union(b); const common = a.intersection(b); const different = a.difference(b);
  const order = combined.values(); a.clear(); b.clear();
  if (order[0] == 'same' && order[1] == 'left' && order[2] == 'right' &&
      common.has('same') && different.has('left') && combined.length == 3) { return 42; }
  return 0;
})"));

class InvalidStandardApiTest : public testing::TestWithParam<std::string_view> {};
TEST_P(InvalidStandardApiTest, RejectsInvalidContracts) {
  EXPECT_FALSE(frontend::CheckSource(GetParam()).Succeeded());
}
INSTANTIATE_TEST_SUITE_P(
    StrictSignatures,
    InvalidStandardApiTest,
    testing::Values("func main(): void { [1].sort((a: int, b: int): bool => true); }",
                    "func main(): void { [1].forEach((a: int): int => a); }",
                    "func main(): void { [1].any((a: double): bool => true); }",
                    "func main(): void { [1].transform((a: int): double => 1.0); }",
                    "func main(): void { [1].reduce(0.0, (a: int, b: int): int => a + b); }",
                    "func main(): void { [1].sort(); }",
                    "func main(): void { Set<int>().union(Set<double>()); }",
                    "func main(): void { Math.pi = 0.0; }",
                    "func main(): void { Math(); }",
                    "func main(): void { Console(); }",
                    "import 'std:collections'; func main(): void {}",
                    "import 'std:math'; func main(): void {}"));

TEST(StandardLibraryTest, DoesNotAddStandardDeclarationsForNamesInsideStringsOrComments) {
  const auto source = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = "// Math Error\nfunc main(): string { return 'Console Random Time'; }",
      },
  });
  const auto loaded = frontend::LoadSources(source, "main.vela");
  ASSERT_TRUE(loaded.diagnostics.empty());
  EXPECT_EQ(loaded.program.sources.size(), 1U);
  EXPECT_TRUE(loaded.program.classes.empty());
}

auto HostClock(runtime::NativeContext& /*unused*/) noexcept -> std::int64_t {
  constexpr std::int64_t kHostClockValue = 42;
  return kHostClockValue;
}
TEST(StandardLibraryTest, ValidatesStaticExternalBindingsAndUsesExplicitHostRegistration) {
  runtime::Isolate isolate;
  ASSERT_TRUE(
      isolate.RegisterNative(runtime::NativeFunction::Bind<HostClock>("Time.monotonicMicros")));
  const auto compiled =
      CompileSource("func main(): int { return Time.monotonicMicros(); }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
  EXPECT_FALSE(CompileSource("class Host { static external func missing(): int = 'missing'; } func "
                             "main(): int { return Host.missing(); }",
                             isolate)
                   .Succeeded());
  EXPECT_FALSE(CompileSource("class Host { static external func clock(value: int): int = "
                             "'Time.monotonicMicros'; } func main(): int { return Host.clock(1); }",
                             isolate)
                   .Succeeded());
}

}  // namespace
}  // namespace vela::compiler
