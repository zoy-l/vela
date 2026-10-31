#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>

#include "gtest/gtest.h"

#include "compiler/test_support/program_test_support.h"
#include "compiler/ir/preparation_test_support.h"
#include "compiler/frontend/modules/source_loader.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler {
namespace {

class LambdaInferenceTest : public testing::TestWithParam<std::string_view> {};

TEST_P(LambdaInferenceTest, ChecksAndExecutesWithPreciseTypesUnderCollectionPressure) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(GetParam(), isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

INSTANTIATE_TEST_SUITE_P(
    ExpectedTypesAndBodies,
    LambdaInferenceTest,
    testing::Values(
        R"(func main(): int { const add: (int, int) => int = (a, b) => a + b; return add(20, 22); })",
        R"(func apply(value: int, operation: (int) => int): int { return operation(value); }
           func main(): int { return apply(operation: x => x * 2, value: 21); })",
        R"(func make(): (int) => (int) => int { return x => y => x + y; }
           func main(): int { return make()(20)(22); })",
        R"(func main(): int { const twice = (x: int) => x * 2; return twice(21); })",
        R"(func main(): int {
             const twice = (x: int) => { if (x > 0) { return x * 2; } return 0; };
             return twice(21);
           })",
        R"(func main(): int {
             let value = 40; const advance = () => { value = value + 2; };
             advance(); const empty = () => {}; empty(); return value;
           })",
        R"(func main(): int {
             let f: (int) => int = x => x;
             f = (x) => x * 2; return f(21);
           })",
        R"(const twice: (int) => int = x => x * 2;
           func main(): int { return twice(21); })",
        R"(class Callback { operation: (int) => int = x => x * 2; }
           func main(): int { return Callback().operation(21); })",
        R"(class Callback { operation: (int) => int; }
           func main(): int { return Callback(operation: x => x * 2).operation(21); })",
        R"(func main(): int {
             const f: ((int) => int)? = x => x * 2;
             if (f != null) { return f(21); } return 0;
           })",
        R"(func main(): int {
             const callbacks: ((int) => int)[] = [x => x + 1, x => x * 2];
             return callbacks[0](20) + callbacks[1](10) + 1;
           })",
        R"(func apply<T>(value: T, operation: (T) => T): T { return operation(value); }
           func main(): int { return apply<int>(21, x => x * 2); })",
        R"(interface Apply { apply(operation: (int) => int): int; }
           class Runner implements Apply {
             apply(operation: (int) => int): int { return operation(21); }
           }
           func main(): int { const runner: Apply = Runner(); return runner.apply(x => x * 2); })",
        R"(func main(): int {
             const optional = (present: bool) => { if (present) { return 42; } return null; };
             const value = optional(true); if (value != null) { return value; } return 0;
           })",
        R"(func main(): int {
             const optional = (present: bool) => { if (!present) { return null; } return 42; };
             const value = optional(true); if (value != null) { return value; } return 0;
           })",
        R"(func main(): int {
             const f: (bool) => int? = flag => { if (flag) { return 42; } return null; };
             const value = f(true); if (value != null) { return value; } return 0;
           })",
        R"(func main(): int {
             const values = [3, 1, 2]; values.sort((left, right) => left - right);
             let total = 0; values.forEach(value => { total = total + value; });
             if (!values.some(value => value == 2) || !values.every(value => value > 0) ||
                 values.findIndex(value => value == 3) != 2) { return 0; }
             const mapped = values.map(value => value * 2);
             return mapped.reduce((sum, value) => sum + value, 24) + total;
           })",
        R"(func main(): int {
             const values = [1, 2, 3];
             const text = values.reduce(callbackFn: (text, value) => text + value, initialValue: '');
             if (text == '123') { return 42; } return 0;
           })",
        R"(func main(): int {
             const values = [1, 2];
             const f = values.reduce((operation, value) => operation, () => 42);
             return f();
           })",
        R"(func main(): int {
             const callbacks: ((int) => int)[] = [];
             const f: () => ((int) => int)[] = () => [x => x * 2];
             return f()[0](21) + callbacks.length;
           })",
        R"(class Box<T> { value: T; } func main(): int {
             const result: Box<(int) => int> = Box<(int) => int>(value: x => x * 2);
             return result.value(21);
           })"));

class LambdaInferenceErrorTest : public testing::TestWithParam<std::string_view> {};

TEST_P(LambdaInferenceErrorTest, RejectsMissingContextAndInconsistentConstraints) {
  runtime::Isolate isolate;
  EXPECT_FALSE(test::CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    StaticConstraints,
    LambdaInferenceErrorTest,
    testing::Values(
        "func main(): void { const f = x => x; }",
        "func main(): void { const f = (x, y: int) => y; }",
        "func main(): void { const f: (int) => int = (x: string) => x.length; }",
        "func main(): void { const f: (int) => int = x => true; }",
        "func main(): void { const f: (int, int) => int = x => x; }",
        "func main(): void { const f: (int) => int = (x, y) => x; }",
        "func main(): void { const f = (x: int) => { if (x > 0) { return x; } }; }",
        "func main(): void { const f = (x: int) => { if (x > 0) { return 1; } return 1.5; }; }",
        "func main(): void { const f = (x: int) => { if (x > 0) { return; } return 1; }; }",
        "func main(): void { const f = () => null; }",
        "func main(): void { const f: () => int = () => null; }",
        "func main(): void { [1, 2].sort((a, b) => a < b); }",
        "func main(): void { [1, 2].map(x => {}); }",
        "func main(): void { const f = ("));

TEST(LambdaInferenceCodegenTest, ProducesTheSameTypedIRAsExplicitAnnotations) {
  constexpr std::string_view kExplicit = R"(
func main(): int {
  const values = [3, 1, 2]; let total = 0;
  values.sort((left: int, right: int): int => left - right);
  values.forEach((value: int): void => { total = total + value; });
  return values.reduce((sum: int, value: int): int => sum + value, 36);
})";
  constexpr std::string_view kInferred = R"(
func main(): int {
  const values = [3, 1, 2]; let total = 0;
  values.sort((left, right) => left - right);
  values.forEach(value => { total = total + value; });
  return values.reduce((sum, value) => sum + value, 36);
})";
  const auto annotated = ir::test::Prepare(kExplicit);
  const auto inferred = ir::test::Prepare(kInferred);
  ASSERT_TRUE(annotated.Succeeded());
  ASSERT_TRUE(inferred.Succeeded());
  const auto& expected = annotated.program.functions;
  const auto& actual = inferred.program.functions;
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto& left = expected.at(index);
    const auto& right = actual.at(index);
    EXPECT_EQ(std::tie(left.register_count, left.parameter_count),
              std::tie(right.register_count, right.parameter_count));
    ASSERT_EQ(left.instructions.size(), right.instructions.size());
    for (std::size_t position = 0; position < left.instructions.size(); ++position) {
      EXPECT_EQ(ir::test::InstructionOperands(left.instructions.at(position)),
                ir::test::InstructionOperands(right.instructions.at(position))) << left.name;
    }
  }
}

TEST(LambdaInferenceTest, PropagatesImportedGenericSignaturesIntoCallbacks) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(import 'library';
        func main(): int { return apply<int>(21, value => value * 2); })",
      },
      {
          .path = "library.vela",
          .text = R"(export func apply<T>(value: T, callback: (T) => T): T {
        return callback(value);
      })",
      },
  });
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileCleanModules(sources, "main.vela", isolate);
  ASSERT_TRUE(compiled) << compiled.error();
  const auto result = test::ExecuteInteger(**compiled);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

}  // namespace
}  // namespace vela::compiler
