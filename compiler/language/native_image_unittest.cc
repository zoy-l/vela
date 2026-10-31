#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "compiler/test_support/program_test_support.h"
#include "compiler/test_support/test_sources.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_program_state.h"

namespace vela::compiler {
namespace {

class NativeImageExecutionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(NativeImageExecutionTest, RestoresOwnedProgramInFreshIsolate) {
  runtime::Isolate isolate(1);
  const auto program = test::ReinstantiateSource(GetParam(), isolate);
  ASSERT_TRUE(program) << program.error();
  const auto result = test::ExecuteInteger(**program);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

INSTANTIATE_TEST_SUITE_P(
    ProductionPaths,
    NativeImageExecutionTest,
    testing::Values(
        test::kMapLiteralSource,
        "func main(): int { return 42; }",
        "func main(): int { return (4000000000 - 3999999958); }",
        "func main(): int { if (1.5 * 2.0 == 3.0) { return 42; } return 0; }",
        "func main(): int { const values: string[] = [\"你好\", \"hello\"]; "
        "if (values.join(\" \") == \"你好 hello\") { return 42; } return 0; }",
        "func main(): int { if ('answer=' + 42 == 'answer=42' && '${true}' == 'true' && "
        "[1, 2].join(',') == '1,2') { return 42; } return 0; }",
        "class Box { value: int; } func main(): int { return Box(value: 42).value; }",
        "func main(): int { const values: int?[][] = [[null], [42]]; "
        "const value=values[1][0]; if(value != null) { return value; } return 0; }",
        "func main(): int { let value = 40; const next: () => int = (): int => { value=value+1; "
        "return value; }; "
        "next(); return next(); }",
        "class Box<T> { value: T; } func main(): int { return Box<int>(value: 42).value; }",
        "interface Readable { read(): int; } class Box implements Readable { read(): int { return "
        "42; } } "
        "func main(): int { const item: Readable = Box(); return item.read(); }",
        "func main(): int { const values: Map<string, Set<int>> = Map<string, Set<int>>(); "
        "const set: Set<int> = Set<int>(); set.add(42); values.set(\"key\", set); "
        "const found = values.get(\"key\"); if (found != null) { return found.values()[0]; } "
        "return 0; }",
        "func main(): int { return Math.sqrt(1764.0).toInt(); }",
        "func main(): int { try { let zero=0; return 1/zero; } "
        "catch (error: Error) { if (error.message == \"division by zero\") { return 42; } return "
        "0; } }",
        "class Failure { value: int; } func main(): int { "
        "try { try { throw Failure(value: 42); } finally { const text = \"allocated\" + \" text\"; "
        "} } "
        "catch (error: Failure) { return error.value; } }",
        "let value = 40; func main(): int { value=value+2; return value; }",
        "func main(): int { const values = [2, 40]; values.sort((a: int, b: int): int => a - b); "
        "return values.reduce((total: int, value: int): int => total + value, 0); }",
        "func main(): int { const values = [2.0, 10.0, 1.0]; values.sort(); "
        "const erased: dynamic = values; erased.sort(); "
        "return values.join() == '1,10,2' ? 42 : 0; }",
        "func main(): int { const a = Set<int>(); const b = Set<int>(); a.add(40); b.add(2); "
        "const values = a.union(b).values(); return values[0] + values[1]; }"));

TEST(NativeImageExecutionTest, IndependentInstancesStartFreshGlobals) {
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileSource(
      "let value = 40; func main(): int { value += 2; return value; }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto first = test::ExecuteInteger(*compiled.instance);
  ASSERT_TRUE(first) << first.error();
  EXPECT_EQ(*first, 42);
  const auto& state = runtime::Heap::NativeProgram(*compiled.instance->state().front().AsObject());
  auto second = runtime::NativeInstance::Instantiate(isolate, state.image_owner);
  ASSERT_TRUE(second);
  const auto fresh = test::ExecuteInteger(**second);
  ASSERT_TRUE(fresh) << fresh.error();
  EXPECT_EQ(*fresh, 42);
  const auto resumed = test::ExecuteInteger(*compiled.instance);
  ASSERT_TRUE(resumed) << resumed.error();
  EXPECT_EQ(*resumed, 44);
}

}  // namespace
}  // namespace vela::compiler
