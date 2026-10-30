#include <algorithm>
#include <array>
#include <string_view>

#include "compiler/frontend/checker.h"
#include "compiler/frontend/diagnostic.h"
#include "gtest/gtest.h"

#include "compiler/frontend/checker_test_support.h"

namespace vela::frontend {
namespace {

TEST(FrontendTest, AcceptsNestedConstructorsAndQualifiedEnum) {
  constexpr std::string_view valid_source = R"(
enum Brightness {
  light,
  dark,
}

class Theme {
  brightness: Brightness;
}

class App {
  theme: Theme;
}

func build(): App {
  return App(theme: Theme(brightness: Brightness.light));
}
)";
  const CheckResult result = CheckTestSource(valid_source);
  for (const Diagnostic& diagnostic : result.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ':' << diagnostic.location.column << ": "
                  << diagnostic.message;
  }
  EXPECT_TRUE(result.Succeeded());
}

TEST(FrontendTest, RejectsUnknownEnumMember) {
  constexpr std::string_view invalid_source = R"(
enum Brightness {
  light,
}

class Theme {
  brightness: Brightness;
}

func build(): Theme {
  return Theme(brightness: Brightness.missing);
}
)";
  const CheckResult result = CheckTestSource(invalid_source);
  EXPECT_FALSE(result.Succeeded());
  EXPECT_FALSE(result.diagnostics.empty());
}

TEST(FrontendTest, RejectsRemovedResultBuiltins) {
  constexpr auto kSources = std::to_array<std::string_view>({
      "func main(value: Expected<int, string>): void {}",
      "func main(): void { const value = ok(42); }",
      "func main(): void { const value = err('failed'); }",
  });
  for (const auto source : kSources) {
    SCOPED_TRACE(source);
    const auto result = CheckTestSource(source);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.diagnostics.empty());
  }
}

TEST(FrontendTest, RejectsDuplicateEnumMember) {
  const CheckResult result = CheckTestSource("enum Colour { red, red }");
  EXPECT_FALSE(result.Succeeded());
  ASSERT_EQ(result.diagnostics.size(), 1U);
  EXPECT_EQ(result.diagnostics.front().message, "duplicate enum member 'red'");
}

TEST(FrontendTest, RejectsDuplicateLocalName) {
  constexpr std::string_view source = R"(
func main(): int {
  let local_with_a_long_name: int = 1;
  let local_with_a_long_name: int = 2;
  return local_with_a_long_name;
}
)";
  const CheckResult result = CheckTestSource(source);
  EXPECT_FALSE(result.Succeeded());
  ASSERT_EQ(result.diagnostics.size(), 1U);
  EXPECT_EQ(result.diagnostics.front().message, "duplicate local name 'local_with_a_long_name'");
}

TEST(FrontendTest, RejectsCoalesceAssignment) {
  constexpr auto kInvalidSources = std::to_array<std::string_view>({
      R"(func main(): void { let value: int? = null; value ??= 42; })",
      R"(let value: int? = null; func main(): void { value ??= 42; })",
      R"(class Box { value: int?; }
         func main(): void { const box = Box(value: null); box.value ??= 42; })",
      R"(func main(): void { const values: int?[] = [null]; values[0] ??= 42; })",
  });
  for (const auto source : kInvalidSources) {
    SCOPED_TRACE(source);
    EXPECT_FALSE(CheckTestSource(source).Succeeded());
  }
}

TEST(FrontendTest, ChecksAsyncFutureReturnAndAwaitContext) {
  constexpr std::string_view source = R"(
external func fetch(): Future<int>;

async func main(): Future<int> {
  return await fetch();
}
)";
  const CheckResult result = CheckTestSource(source);
  EXPECT_TRUE(result.Succeeded()) << (result.diagnostics.empty()
                                          ? ""
                                          : result.diagnostics.front().message);
}

TEST(FrontendTest, RejectsAwaitOutsideAsyncFunction) {
  constexpr std::string_view source = R"(
external func fetch(): Future<int>;

func main(): int {
  return await fetch();
}
)";
  const CheckResult result = CheckTestSource(source);
  EXPECT_FALSE(result.Succeeded());
  EXPECT_NE(std::ranges::find_if(result.diagnostics,
                                 [](const Diagnostic& diagnostic) {
                                   return diagnostic.message ==
                                          "await is only valid inside an async function";
                                 }),
            result.diagnostics.end());
}

TEST(FrontendTest, RejectsArrayTypeMismatchesWithoutConversions) {
  constexpr auto kInvalidSources = std::to_array<std::string_view>({
      "func main(): int { let a: int[] = [1, true]; return 0; }",
      "func main(): int { let a: double[] = [1]; return 0; }",
      "func main(): int { let a: int[] = [1]; let b: bool[] = a; return 0; }",
      "func main(): int { let a: int[] = [1]; a[0] = true; return 0; }",
      "func main(): int { let a: int[] = [1]; return a[true]; }",
      "func main(): int { let a: int[][] = [1]; return 0; }",
      "func f(): int[] { return [true]; }",
      "func f(a: bool[]): void {} func main(): int { f([1]); return 0; }",
      "func main(): int { let a: void[] = []; return 0; }",
      "func main(): int { let a: Unknown[] = []; return 0; }",
      "func main(): int { []; return 0; }",
      "func main(): int { let a: int = [1]; return 0; }",
      "enum E { a } func main(): int { let a: E[] = [1]; return 0; }",
      "class A {} class B {} func main(): int { let a: A[] = [B()]; return 0; }",
      "func main(): int { let a: int[] = [1]; a = [false]; return 0; }",
      "func main(): int { let a: int[] = [1]; let equal: bool = a == a; return 0; }",
      "func f(): void {} func main(): int { [f()]; return 0; }",
  });
  for (const auto source : kInvalidSources) {
    SCOPED_TRACE(source);
    EXPECT_FALSE(CheckTestSource(source).Succeeded());
  }
}

TEST(FrontendTest, PreservesArrayRankAndContextualElements) {
  constexpr std::string_view source = R"(
enum E { a, b }
func f(input: E[][]): E[][] { return input; }
func main(): int {
  let values: E[][] = [[], [E.a, E.b]];
  values[0] = [E.b];
  let enabled = false;
  for (let copy: E[][] = f(values); enabled;) { copy[0][0] = E.a; }
  let exact: double[] = [1.5];
  return [1, 2][0];
}
)";
  const auto checked = CheckTestSource(source);
  for (const auto& diagnostic : checked.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  EXPECT_TRUE(checked.Succeeded());
}

TEST(FrontendTest, RejectsImplicitDoubleConversionsAtEveryBoundary) {
  constexpr auto kInvalidSources = std::to_array<std::string_view>({
      "func main(): double { return 1; }",
      "func main(): int { return 1.0; }",
      "func main(): int { let x: double = 1; return 0; }",
      "func main(): int { let x: int = 1.0; return 0; }",
      "func main(): int { let x: double = 1.0; x = 2; return 0; }",
      "func main(): int { let x: int = 1; x = 2.0; return 0; }",
      "func f(x: double): double { return x; } func main(): double { return f(1); }",
      "class A { x: double; } func main(): A { return A(x: 1); }",
      "class A { f(x: double): double { return x; } } func main(): double { return A().f(1); }",
      "func main(): double { return 1 + 1.0; }",
      "func main(): double { return 1.0 + 1; }",
      "func main(): bool { return 1 == 1.0; }",
      "func main(): bool { return 1.0 != 1; }",
      "func main(): bool { return 1 < 1.0; }",
      "func main(): bool { return 1.0 >= 1; }",
      "func main(): int { let x: double[] = [1.0]; x[0] = 1; return 0; }",
      "func main(): int { let x: double[] = [1.0]; return x[0.0]; }",
      "func main(): int { let x: double[] = [1.0]; let y: int[] = x; return 0; }",
      "func main(): int { let x: double[] = [1.0, 2]; return 0; }",
      "func main(): int { if (1.0) return 1; return 0; }",
      "func main(): int { let x: double = 1e309; return 0; }",
      "func main(): int { let x: double = 1e-9999; return 0; }",
      "func main(): int { let x: double = 1e+; return 0; }",
      "func main(): int { let x: num = 1; return 0; }",
      "func main(): int { let x: float = 1.0; return 0; }",
  });
  for (const auto source : kInvalidSources) {
    SCOPED_TRACE(source);
    EXPECT_FALSE(CheckTestSource(source).Succeeded());
  }
}

}  // namespace
}  // namespace vela::frontend
