#include <array>
#include <string_view>

#include "compiler/frontend/diagnostic.h"
#include "gtest/gtest.h"

#include "compiler/frontend/checker.h"

namespace vela::frontend {
namespace {

TEST(FrontendTest, AcceptsNestedConstructorsAndContextualEnum) {
  constexpr std::string_view valid_source = R"(
enum Brightness {
  light,
  dark,
}

class Theme {
  Brightness brightness;
}

class App {
  Theme theme;
}

App build() {
  return App(theme: Theme(brightness: .light));
}
)";
  const CheckResult result = CheckSource(valid_source);
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
  Brightness brightness;
}

Theme build() {
  return Theme(brightness: .missing);
}
)";
  const CheckResult result = CheckSource(invalid_source);
  EXPECT_FALSE(result.Succeeded());
  EXPECT_FALSE(result.diagnostics.empty());
}

TEST(FrontendTest, RejectsDuplicateEnumMember) {
  const CheckResult result = CheckSource("enum Colour { red, red }");
  EXPECT_FALSE(result.Succeeded());
  ASSERT_EQ(result.diagnostics.size(), 1U);
  EXPECT_EQ(result.diagnostics.front().message, "duplicate enum member 'red'");
}

TEST(FrontendTest, RejectsDuplicateLocalName) {
  constexpr std::string_view source = R"(
int main() {
  int local_with_a_long_name = 1;
  int local_with_a_long_name = 2;
  return local_with_a_long_name;
}
)";
  const CheckResult result = CheckSource(source);
  EXPECT_FALSE(result.Succeeded());
  ASSERT_EQ(result.diagnostics.size(), 1U);
  EXPECT_EQ(result.diagnostics.front().message, "duplicate local name 'local_with_a_long_name'");
}

TEST(FrontendTest, RejectsArrayTypeMismatchesWithoutConversions) {
  constexpr auto kInvalidSources = std::to_array<std::string_view>({
      "int main() { int[] a = [1, true]; return 0; }",
      "int main() { double[] a = [1]; return 0; }",
      "int main() { int[] a = [1]; bool[] b = a; return 0; }",
      "int main() { int[] a = [1]; a[0] = true; return 0; }",
      "int main() { int[] a = [1]; return a[true]; }",
      "int main() { int[][] a = [1]; return 0; }",
      "int[] f() { return [true]; }",
      "void f(bool[] a) {} int main() { f([1]); return 0; }",
      "int main() { void[] a = []; return 0; }",
      "int main() { Unknown[] a = []; return 0; }",
      "int main() { []; return 0; }",
      "int main() { int a = [1]; return 0; }",
      "enum E { a } int main() { E[] a = [1]; return 0; }",
      "class A {} class B {} int main() { A[] a = [B()]; return 0; }",
      "int main() { int[] a = [1]; a = [false]; return 0; }",
      "int main() { int[] a = [1]; bool equal = a == a; return 0; }",
      "void f() {} int main() { [f()]; return 0; }",
  });
  for (const auto source : kInvalidSources) {
    SCOPED_TRACE(source);
    EXPECT_FALSE(CheckSource(source).Succeeded());
  }
}

TEST(FrontendTest, PreservesArrayRankAndContextualElements) {
  constexpr std::string_view source = R"(
enum E { a, b }
E[][] f(E[][] input) { return input; }
int main() {
  E[][] values = [[], [.a, .b]];
  values[0] = [.b];
  for (E[][] copy = f(values); false;) { copy[0][0] = .a; }
  double[] exact = [1.5];
  return [1, 2][0];
}
)";
  const auto checked = CheckSource(source);
  for (const auto& diagnostic : checked.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  EXPECT_TRUE(checked.Succeeded());
}

TEST(FrontendTest, RejectsImplicitDoubleConversionsAtEveryBoundary) {
  constexpr auto kInvalidSources = std::to_array<std::string_view>({
      "double main() { return 1; }",
      "int main() { return 1.0; }",
      "int main() { double x = 1; return 0; }",
      "int main() { int x = 1.0; return 0; }",
      "int main() { double x = 1.0; x = 2; return 0; }",
      "int main() { int x = 1; x = 2.0; return 0; }",
      "double f(double x) { return x; } double main() { return f(1); }",
      "class A { double x; } A main() { return A(x: 1); }",
      "class A { double f(double x) { return x; } } double main() { return A().f(1); }",
      "double main() { return 1 + 1.0; }",
      "double main() { return 1.0 + 1; }",
      "bool main() { return 1 == 1.0; }",
      "bool main() { return 1.0 != 1; }",
      "bool main() { return 1 < 1.0; }",
      "bool main() { return 1.0 >= 1; }",
      "int main() { double[] x = [1.0]; x[0] = 1; return 0; }",
      "int main() { double[] x = [1.0]; return x[0.0]; }",
      "int main() { double[] x = [1.0]; int[] y = x; return 0; }",
      "int main() { double[] x = [1.0, 2]; return 0; }",
      "int main() { if (1.0) return 1; return 0; }",
      "int main() { double x = 1e309; return 0; }",
      "int main() { double x = 1e-9999; return 0; }",
      "int main() { double x = 1e+; return 0; }",
      "int main() { num x = 1; return 0; }",
      "int main() { float x = 1.0; return 0; }",
  });
  for (const auto source : kInvalidSources) {
    SCOPED_TRACE(source);
    EXPECT_FALSE(CheckSource(source).Succeeded());
  }
}

}  // namespace
}  // namespace vela::frontend
