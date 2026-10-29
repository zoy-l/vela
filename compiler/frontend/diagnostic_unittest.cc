#include <algorithm>
#include <cstdint>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/frontend/checker_test_support.h"
#include "compiler/frontend/parser.h"

namespace vela::frontend {
namespace {

struct LocatedError final {
  std::string_view source;
  std::string_view message;
  std::string_view token;
};

class SourceDiagnosticTest : public testing::TestWithParam<LocatedError> {};

TEST_P(SourceDiagnosticTest, PointsToTheResponsibleToken) {
  const auto& parameter = GetParam();
  SCOPED_TRACE(parameter.source);
  const auto offset = parameter.source.rfind(parameter.token);
  ASSERT_NE(offset, std::string_view::npos);
  const auto prefix = parameter.source.substr(0, offset);
  const auto line = static_cast<std::uint32_t>(std::ranges::count(prefix, '\n') + 1);
  const auto newline = prefix.rfind('\n');
  const auto column =
      static_cast<std::uint32_t>(newline == std::string_view::npos ? offset + 1 : offset - newline);
  const auto checked = CheckTestSource(parameter.source);
  EXPECT_TRUE(std::ranges::any_of(checked.diagnostics, [&](const auto& diagnostic) -> bool {
    return diagnostic.message.contains(parameter.message) && diagnostic.location.line == line &&
           diagnostic.location.column == column;
  }));
}

INSTANTIATE_TEST_SUITE_P(
    LexerParserAndChecker,
    SourceDiagnosticTest,
    testing::Values(
        LocatedError{"\n@", "unexpected character", "@"},
        LocatedError{"\n/* unfinished", "unterminated block comment", "/*"},
        LocatedError{"func f(): string { return \"\\q\"; }", "invalid string escape", "q"},
        LocatedError{"func f(): void {\n  let value = 1\n}", "expected ';'", "}"},
        LocatedError{"func f(): void {", "expected '}'", ""},
        LocatedError{"func f(): void {\n  let value = ;\n}", "expected expression", ";"},
        LocatedError{"func f(): void {\n  let value: Missing = 1;\n}", "unknown type", "Missing"},
        LocatedError{"enum E { a,\n  a }", "duplicate enum member", "a"},
        LocatedError{"class A { x: int;\n  x: int; }", "duplicate field", "x"},
        LocatedError{"class A { read(): void {}\n  read(): void {} }", "duplicate method", "read"},
        LocatedError{"func f(x: int,\n  x: int): void {}", "duplicate parameter", "x"},
        LocatedError{"func f(): void {}\nfunc f(): void {}", "duplicate function", "f"},
        LocatedError{"func f(): void {\n  let value = 1;\n  let value = 2;\n}", "duplicate local",
                     "value"},
        LocatedError{"func f(): int {\n  return missing;\n}", "unknown name", "missing"},
        LocatedError{"func f(): void {\n  const value = 1;\n  value = 2;\n}", "cannot assign",
                     "value"},
        LocatedError{"func f(): int {\n  return true;\n}", "cannot return", "return"},
        LocatedError{"func f(): void {\n  let value: int = true;\n}", "cannot initialize", "value"},
        LocatedError{"func read(value: int): void {}\nfunc f(): void { read(true); }",
                     "argument type", "true"},
        LocatedError{"func read(value: int): void {}\nfunc f(): void { read(other: 1); }",
                     "unknown named", "other"},
        LocatedError{"func read(value: int): void {}\nfunc f(): void { read(); }",
                     "missing argument", "read"},
        LocatedError{"class A { x: int; }\nfunc f(): A { return A(other: 1); }", "unknown named",
                     "other"},
        LocatedError{"func f(): int {\n  return 1 + true;\n}", "arithmetic operands", "+"},
        LocatedError{"func f(): int {\n  return [1][true];\n}", "index must be int", "true"},
        LocatedError{"func f(flag: bool?): void {\n  if (flag) {}\n}", "condition must be bool",
                     "flag"},
        LocatedError{"func f(value: string?): int {\n  return value.length;\n}", "non-null check",
                     "value"},
        LocatedError{"func f(): void {\n  break;\n}", "inside a loop or switch", "break"},
        LocatedError{"func f(): void {\n  return;\n  print(1);\n}", "unreachable", "print"},
        LocatedError{"func f(): void { switch (1) {\n  case 1: break;\n  case 1: break; } }",
                     "duplicate case", "1:"}));

TEST(ParserRecoveryTest, PreservesTheNextFunctionAfterAMissingClosingBrace) {
  const auto parsed = ParseSource("func first(): void {\n  return;\nfunc second(): void {}\n");
  ASSERT_FALSE(parsed.diagnostics.empty());
  ASSERT_EQ(parsed.program.functions.size(), 1U);
  EXPECT_EQ(parsed.program.functions.front().name, "second");
  EXPECT_EQ(parsed.diagnostics.front().location.line, 3U);
}

TEST(ParserRecoveryTest, MissingInitializerDoesNotConsumeTheClosingBrace) {
  const auto parsed = ParseSource("func first(): void { let value = }\nfunc second(): void {}\n");
  ASSERT_FALSE(parsed.diagnostics.empty());
  ASSERT_EQ(parsed.program.functions.size(), 2U);
  EXPECT_EQ(parsed.program.functions.back().name, "second");
}

TEST(ParserRecoveryTest, HandlesTruncatedLambdaAndGenericLookaheadAtEof) {
  for (const auto source : {"func f(): void { (x", "func f(): void { (x)",
                            "func f(): void { call<int>", "func f(): void { call<Box<int>>"}) {
    SCOPED_TRACE(source);
    EXPECT_FALSE(ParseSource(source).diagnostics.empty());
  }
}

TEST(ParserRecoveryTest, ParsesLongComparisonChainsAndNestedGenericClosings) {
  std::string source = "func f(a: int, b: int): bool { return ";
  constexpr int kComparisons = 1024;
  for (int index = 0; index < kComparisons; ++index) {
    source += index == 0 ? "a < b" : " && a < b";
  }
  source += "; }";
  EXPECT_TRUE(ParseSource(source).diagnostics.empty());
  EXPECT_TRUE(
      ParseSource("func f(): void { call<Map<string, Map<int, int>>>(); }").diagnostics.empty());
}

}  // namespace
}  // namespace vela::frontend
