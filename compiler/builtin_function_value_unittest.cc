#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "vm/bytecode_image.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

class BuiltinFunctionValueTest : public testing::TestWithParam<std::string_view> {};

TEST_P(BuiltinFunctionValueTest, PreservesReceiversAndReleasesThemAfterExecution) {
  runtime::Isolate isolate(1);
  const auto retained = isolate.heap().object_count();
  {
    const auto compiled = CompileSource(GetParam(), isolate);
    for (const auto& diagnostic : compiled.diagnostics) {
      ADD_FAILURE() << diagnostic.location.line << ':' << diagnostic.message;
    }
    ASSERT_TRUE(compiled.Succeeded());
    const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
    ASSERT_TRUE(result.Succeeded()) << result.message;
    ASSERT_TRUE(result.value.IsInteger());
    EXPECT_EQ(result.value.AsInteger(), 42);
  }
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), retained);
}

TEST_P(BuiltinFunctionValueTest, ReloadsInFreshIsolateWithoutCompilerMetadata) {
  std::vector<std::byte> image;
  {
    runtime::Isolate isolate(1);
    const auto compiled = CompileSource(GetParam(), isolate);
    ASSERT_TRUE(compiled.Succeeded());
    const auto saved = vm::SaveBytecode(compiled.program, isolate);
    ASSERT_TRUE(saved) << saved.error().message;
    image = *saved;
  }
  runtime::Isolate isolate(1);
  const auto program = vm::LoadBytecode(image, isolate);
  ASSERT_TRUE(program) << program.error().message;
  std::ranges::fill(image, std::byte{0});
  isolate.heap().Collect();
  const auto result = vm::VirtualMachine(isolate).Execute(program->FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(ProductionPaths,
                         BuiltinFunctionValueTest,
                         testing::Values(
                             R"(
func main(): int {
  let value = 42;
  const render = value.toString;
  value = 7;
  let text = 'Vela';
  const contains = text.includes;
  text = 'changed';
  if (render() == '42' && contains('ela')) { return 42; }
  return 0;
})",
                             R"(
func make(): () => string {
  const builder = String();
  const append = builder.append;
  append('Vela');
  return builder.toString;
}
func main(): int { const read = make(); if (read() == 'Vela') { return 42; } return 0; }
)",
                             R"(
func main(): int {
  const values: int[] = [];
  const one: (int) => int = values.push;
  const two: (int, int) => int = values.push;
  const count: () => int = values.push;
  one(40);
  two(1, 1);
  if (count() == 3) { return values[0] + values[1] + values[2]; }
  return 0;
})",
                             R"(
func main(): int {
  const values = [1, 2, 3];
  const replace: (int, int, int) => int[] = values.splice;
  const removed = replace(0, 2, 42);
  if (removed.length == 2 && removed[0] == 1 && values.length == 2) { return values[0]; }
  return 0;
})",
                             R"(
func main(): int {
  const values = [20, 22];
  const tail: (int) => int[] = values.slice;
  const all: () => int[] = values.slice;
  const range = values.slice;
  values.push(42);
  if (all().length == 3 && range(0, 2)[1] == 22) { return tail(2)[0]; }
  return 0;
})",
                             R"(
func main(): int {
  const values = [20, 22];
  const map: ((int) => string) => string[] = values.map;
  const texts = map(value => value.toString());
  const reduce: ((int, int) => int, int) => int = values.reduce;
  if (texts[0] == '20' && texts[1] == '22') { return reduce((sum, value) => sum + value, 0); }
  return 0;
})",
                             R"(
func main(): int {
  const values = [40, 2];
  const sort: () => int[] = values.sort;
  if (sort()[0] != 2) { return 0; }
  const descending: ((int, int) => double) => int[] = values.sort;
  const sorted = descending((left, right) => (right - left).toDouble());
  return sorted[0] + sorted[1];
})",
                             R"(
func main(): int {
  const values = Map<string, int>();
  const set = values.set;
  const get = values.get;
  set('answer', 42);
  return get('answer') ?? 0;
})",
                             R"(
func main(): int {
  const first = Set<int>();
  const add = first.add;
  add(40);
  const second = Set<int>();
  second.add(2);
  const combine = first.union;
  const result = combine(second).values();
  return result[0] + result[1];
})",
                             R"(
func main(): int {
  const result: Expected<int, string> = ok(42);
  const hasValue = result.hasValue;
  const value = result.value;
  if (hasValue()) { return value(); }
  return 0;
})",
                             R"(
func contains(text: string?): int {
  if (text != null) {
    const has = text.includes;
    if (has('ela')) { return text.length + 38; }
  }
  return 0;
}
func main(): int {
  const text: string? = null;
  const absent: ((string) => bool)? = text?.includes;
  if (absent != null) { return 0; }
  return contains('Vela');
})",
                             R"(
class Reader { public read(): int { return 42; } }
func bind(reader: Reader?): (() => int)? { return reader?.read; }
func main(): int {
  if (bind(null) != null) { return 0; }
  const read = bind(Reader());
  if (read != null) { return read(); }
  return 0;
})",
                             R"(
class Base { public read(): int { return 40; } }
class Derived extends Base {
  public readBase(): () => int { return super.read; }
}
func main(): int {
  const object = Derived();
  const inherited = object.read;
  const base = object.readBase();
  return inherited() + base() - 38;
})",
                             R"(
let calls = 0;
const values = [42];
func receiver(): int[] { calls += 1; return values; }
func main(): int {
  const copy: () => int[] = receiver().slice;
  copy(); copy();
  if (calls == 1) { return copy()[0]; }
  return 0;
})"));

class InvalidBuiltinFunctionValueTest : public testing::TestWithParam<std::string_view> {};

TEST_P(InvalidBuiltinFunctionValueTest, RejectsUnresolvedOrIncompatibleSignatures) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(GetParam(), isolate);
  EXPECT_FALSE(compiled.Succeeded());
  EXPECT_FALSE(compiled.diagnostics.empty());
}

INSTANTIATE_TEST_SUITE_P(
    StrictTypes,
    InvalidBuiltinFunctionValueTest,
    testing::Values(
        "func main(): void { const values = [1]; const append = values.push; }",
        "func main(): void { const values = [1]; const map = values.map; }",
        "func main(): void { const has: (string) => int = 'Vela'.includes; }",
        "func main(): void { const has: (string) => void = 'Vela'.includes; }",
        "func main(): void { const copy: (int, int, int) => int[] = [1].slice; }",
        "func main(): void { const get: (int) => int? = Map<string, int>().get; }",
        "func main(): void { const missing: () => int = [1].missing; }",
        "func main(): void { const values: int[]? = null; const copy = values.slice; }"));

}  // namespace
}  // namespace vela::compiler
