#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "compiler/program_test_support.h"
#include "runtime/isolate.h"

namespace vela::compiler {
namespace {

class MatchExpressionTest : public testing::TestWithParam<std::string_view> {};

TEST_P(MatchExpressionTest, EvaluatesSelectorOnceAndExecutesOnlySelectedArm) {
  runtime::Isolate isolate(1);
  const auto result = test::RunInteger(GetParam(), isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

INSTANTIATE_TEST_SUITE_P(ExhaustiveArms,
                         MatchExpressionTest,
                         testing::Values(
                             R"(
enum Brightness { light, dark }
func name(value: Brightness): string {
  return match (value) { Brightness.light => "light", Brightness.dark => "dark", };
}
func main(): int { return match (name(Brightness.dark)) { "dark" => 42, _ => 0 }; }
)",
                             R"(
func value(input: int?): int { return match (input) { null => 0, let present => present }; }
func main(): int { return value(42) + value(null); }
)",
                             R"(
class State {
  static calls: int = 0;
  static next(): bool { State.calls = State.calls + 1; return true; }
}
func main(): int {
  const result: int = match (State.next()) { false => 1 / 0, true => 41 };
  return result + State.calls;
}
)",
                             R"(
func choose(input: int?): () => int {
  return match (input) { null => (): int => 0, let value => (): int => { value = value + 1; return value; } };
}
func main(): int { const f = choose(40); f(); return f(); }
)",
                             R"(
func choose(value: bool): int? {
  return match (value) { true => 42, false => null };
}
func main(): int { return choose(true) ?? 0; }
)",
                             R"(
func choose(value: bool?): int { return match (value) { true => 42, false => 0, null => 1 }; }
func main(): int { return choose(true); }
)",
                             R"(
func main(): int {
  let value: int = 40;
  return value + match (true) { true => value = 2, false => 0 };
}
)"));

class MatchErrorTest : public testing::TestWithParam<std::string_view> {};

TEST_P(MatchErrorTest, RejectsMissingDuplicateAndIncompatiblePatterns) {
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    Exhaustiveness,
    MatchErrorTest,
    testing::Values("enum E { a,b } func main(x: E): int { return match(x) { E.a => 1 }; }",
                    "func main(x: bool?): int { return match(x) { true => 1, false => 0 }; }",
                    "func main(x: int): int { return match(x) { 1 => 1 }; }",
                    "func main(x: int): int { return match(x) { 1 => 1, 1 => 2, _ => 0 }; }",
                    "func main(x: bool): int { return match(x) { _ => 1, true => 0 }; }",
                    "func main(x: bool): int { return match(x) { true => 1, false => 1.0 }; }",
                    "func main(x: bool): int { return match(x) { 1 => 1, _ => 0 }; }",
                    "func main(x: int): int { return match(x) { null => 1, _ => 0 }; }",
                    "func main(): int { return match(null) { null => 1, _ => 0 }; }"));

}  // namespace
}  // namespace vela::compiler
