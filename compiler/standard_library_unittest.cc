#include <array>
#include <sstream>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/frontend/checker.h"
#include "compiler/frontend/source_loader.h"
#include "runtime/isolate.h"
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
import "std:math" as math;
func main(): int {
  if (math.sqrt(9.0) == 3.0 && math.pow(2.0, 3.0) == 8.0 && math.sin(0.0) == 0.0 &&
      math.cos(0.0) == 1.0 && math.floor(2.9) == 2.0 && math.ceil(2.1) == 3.0 &&
      math.round(-1.5) == -2.0 && math.trunc(-1.9) == -1.0 && math.abs(-2.0) == 2.0 &&
      math.min(2.0, 3.0) == 2.0 && math.max(2.0, 3.0) == 3.0 &&
      math.clamp(10.0, 0.0, 5.0) == 5.0 && math.isFinite(math.pi) && math.e > 2.0) {
    return 42;
  }
  return 0;
})",
                             R"(
import "std:math" as math;
import "std:error";
func main(): int {
  let caught = 0;
  try { math.sqrt(-1.0); } catch (error: Error) { caught = caught + 1; }
  try { math.log(0.0); } catch (error: Error) { caught = caught + 1; }
  try { math.clamp(0.0, 2.0, 1.0); } catch (error: Error) { caught = caught + 1; }
  if (caught == 3 && math.isNaN(math.pow(-1.0, 0.5)) && !math.isFinite(math.exp(1000.0))) {
    return 42;
  }
  return 0;
})",
                             R"(
import "std:random" as random;
import "std:error";
func main(): int {
  random.seed(123);
  const first = random.nextInt(1000);
  random.seed(123);
  if (first != random.nextInt(1000)) { return 0; }
  for (let index = 0; index < 500; index = index + 1) {
    const value = random.nextInt(7);
    const fraction = random.nextDouble();
    if (value < 0 || value >= 7 || fraction < 0.0 || fraction >= 1.0) { return 0; }
  }
  try { random.nextInt(0); } catch (error: Error) { return 42; }
  return 0;
})",
                             R"(
import "std:time" as time;
func main(): int {
  const start = time.monotonicMicros();
  const finish = time.monotonicMicros();
  if (finish >= start && time.unixMicros() > 0) { return 42; }
  return 0;
})",
                             R"(
import "std:collections" as collections;
func main(): int {
  const values: int[] = [1, 2, 3];
  let sum = 0;
  collections.forEach<int>(values, (value: int): void => { sum = sum + value; });
  if (sum != 6 || !collections.any<int>(values, (value: int): bool => value == 2) ||
      !collections.every<int>(values, (value: int): bool => value > 0) ||
      collections.findIndex<int>(values, (value: int): bool => value == 3) != 2) { return 0; }
  collections.transform<int>(values, (value: int): int => value * 2);
  return collections.reduce<int, int>(values, 30, (sum: int, value: int): int => sum + value);
})",
                             R"(
import "std:collections" as collections;
func main(): int {
  const a: Set<int> = Set<int>(); const b: Set<int> = Set<int>();
  a.add(1); a.add(2); b.add(2); b.add(3);
  const combined = collections.union<int>(a, b);
  const common = collections.intersection<int>(a, b);
  const different = collections.difference<int>(a, b);
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
import "std:error";
func main(): int {
  const values = [1, 2]; let count = 0;
  try { values.slice(-1, 1); } catch (error: Error) { count = count + 1; }
  try { values.slice(2, 1); } catch (error: Error) { count = count + 1; }
  try { values.slice(0, 3); } catch (error: Error) { count = count + 1; }
  if (count == 3) { return 42; } return 0;
})",
                             R"(
import "std:collections" as collections;
func main(): int {
  const values: int[] = [3, 1, 2, 2];
  collections.sort<int>(values, (left: int, right: int): int => left - right);
  const empty: int[] = [];
  collections.sort<int>(empty, (left: int, right: int): int => left - right);
  if (values[0] == 1 && values[1] == 2 && values[2] == 2 && values[3] == 3) { return 42; }
  return 0;
})"));

TEST(StandardConsoleTest, ReadsUtf8CrLfEmptyLinesAndEofAndRedirectsPrint) {
  runtime::Isolate isolate(1);
  std::istringstream input("你好\r\n\nlast");
  std::ostringstream output;
  isolate.SetConsole(input, output);
  const auto compiled = CompileSource(R"(
import "std:console" as console;
func main(): int {
  console.write("prompt> "); console.flush();
  const first = console.readLine(); const empty = console.readLine(); const last = console.readLine();
  if (first != null && empty == "" && last == "last" && console.readLine() == null) {
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
import "std:console" as console;
import "std:error";
func main(): string {
  try { console.readLine(); } catch (error: Error) { return error.message; }
  return "not caught";
})",
                                      isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsString()->value(), "console input is not valid UTF-8");
}

TEST(StandardLibraryTest, RejectsImplicitConversionsAndSpoofedStandardModules) {
  EXPECT_FALSE(frontend::CheckSource(
                   "import \"std:math\" as math; func main(): double { return math.sqrt(4); }")
                   .Succeeded());
  EXPECT_FALSE(frontend::CheckSource("func main(): void { const values = [1]; values.fill(1.0); }")
                   .Succeeded());
  EXPECT_FALSE(frontend::CheckSource("import \"std:missing\";").Succeeded());
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import \"std:math\";"},
      {.path = "std:math", .text = "export func sqrt(value: double): double { return 0.0; }"},
  });
  EXPECT_FALSE(frontend::CheckModules(sources, "main.vela").Succeeded());
}

}  // namespace
}  // namespace vela::compiler
