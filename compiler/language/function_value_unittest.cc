#include <string_view>

#include "gtest/gtest.h"

#include "compiler/bytecode/compiler.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

class FunctionValueTest : public testing::TestWithParam<std::string_view> {};

TEST_P(FunctionValueTest, ExecutesUnderCollectionPressure) {
  runtime::Isolate isolate(1);
  const auto program = test::CompileCleanSource(GetParam(), isolate);
  ASSERT_TRUE(program) << program.error();
  const auto retained = isolate.heap().object_count();
  const auto result = test::ExecuteInteger(*program, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), retained);
}

INSTANTIATE_TEST_SUITE_P(LexicalBindings,
                         FunctionValueTest,
                         testing::Values(
                             R"(
type Binary = (int, int) => int;
func add(left: int, right: int): int { return left + right; }
func main(): int { const f: Binary = add; return f(20, 22); }
)",
                             R"(
func apply(value: int, operation: (int) => int): int { return operation(value); }
func main(): int { return apply(21, (x: int): int => x * 2); }
)",
                             R"(
func counter(start: int): () => int {
  let current: int = start;
  return (): int => { current = current + 1; return current; };
}
func main(): int { const f = counter(40); f(); return f(); }
)",
                             R"(
func main(): int {
  let x: int = 0;
  const one = (): int => { x = x + 1; return x; };
  const two = (): int => { x = x + 2; return x; };
  one(); two(); x = x + 38;
  return one();
}
)",
                             R"(
func outer(start: int): () => () => int {
  return (): () => int => (): int => { start = start + 1; return start; };
}
func main(): int { return outer(41)()(); }
)",
                             R"(
func main(): int {
  const values: (() => int)[] = [];
  return 42;
}
)",
                             R"(
class Counter {
  value: int;
  next(): int { this.value = this.value + 1; return this.value; }
}
func main(): int {
  const c = Counter(value: 41);
  const f: () => int = c.next;
  return f();
}
)",
                             R"(
func make(): () => int {
  const text: string = "a" + "b";
  return (): int => { const garbage = "x" + text; return text.length + 40; };
}
func main(): int { const f = make(); return f(); }
)",
                             R"(
class Callback { operation: (int) => int; }
func main(): int {
  const object = Callback(operation: (value: int): int => value + 1);
  return object.operation(41);
}
)",
                             R"(
class Callback {
  reads: int = 0;
  get operation(): (int) => int {
    this.reads++;
    return (value: int): int => value + 1;
  }
}
func main(): int {
  const object = Callback();
  const value = object.operation(40);
  return value + object.reads;
}
)",
                             R"(
func main(): int {
  const callbacks: (() => int)[] = [(): int => 40, (): int => 2];
  return callbacks[0]() + callbacks[1]();
}
)",
                             R"(
type Optional = (() => int)?;
func main(): int {
  const f: Optional = (): int => 42;
  if (f != null) { return f(); }
  return 0;
}
)",
                             R"(
func main(): int {
  let f: () => int = (): int => 0;
  for (let i: int = 0; i < 3; i = i + 1) {
    let local: int = i;
    f = (): int => { local = local + 40; return local; };
  }
  return f();
}
)",
                             R"(
func noOp(): void {}
func main(): int { const f: () => void = (): void => noOp(); f(); return 42; }
)",
                             R"(
func main(): int {
  let recursive: ((int) => int)? = null;
  recursive = (count: int): int => {
    if (count == 0) { return 42; }
    const next = recursive;
    if (next != null) { return next(count - 1); }
    return 0;
  };
  const first = recursive;
  if (first != null) { return first(4); }
  return 0;
}
)"));

class FunctionTypeErrorTest : public testing::TestWithParam<std::string_view> {};

TEST_P(FunctionTypeErrorTest, RejectsInvalidSignaturesCapturesAndAliases) {
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    StaticContracts,
    FunctionTypeErrorTest,
    testing::Values(
        "func main(): void { const f: (x: int) => int = (x: int): int => x; }",
        "func main(): void { const f: (int) => double = (x: int): int => x; }",
        "func main(): int { const f = (): int => 42; return f(extra: 1); }",
        "func main(): int { const x: int = 1; const f = (): int => { x = 2; return x; }; return "
        "f(); }",
        "func main(): void { const f = (): int => { if (false) return 1; }; }",
        "type A = B; type B = A; func main(): void {}",
        "type A = void; func main(): void { let x: A = 1; }",
        "func main(): int { const x: int? = null; const f = (): int => x; return f(); }",
        R"(
func main(): int {
  let value: int? = 42;
  if (value != null) {
    let index: int = 0;
    while (index < 2) {
      const clear = (): void => { value = null; };
      const result: int = value + 1;
      clear();
      index = index + 1;
    }
  }
  return 0;
}
)",
        R"(
func main(): int {
  let x: int? = 1;
  const f = (): void => { x = null; };
  if (x != null) { f(); return x; }
  return 0;
}
)"));

TEST(TypeAliasTest, ResolvesNestedArrayAndNullableAliasesTransparently) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(R"(
type Id = int;
type MaybeId = Id?;
type Rows = MaybeId[][];
func main(): int {
  const values: Rows = [[40, null, 2]];
  const value: MaybeId = values[0][0];
  if (value != null) { return value + 2; }
  return 0;
}
)",
                                       isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

}  // namespace
}  // namespace vela::compiler
