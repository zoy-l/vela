#include <algorithm>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/frontend/checker_test_support.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {
namespace {

struct FlowErrorCase final {
  std::string_view source;
  std::string_view message;
};

class ControlFlowErrorTest : public testing::TestWithParam<FlowErrorCase> {};

TEST_P(ControlFlowErrorTest, ReportsTheControlFlowError) {
  const auto& parameter = GetParam();
  SCOPED_TRACE(parameter.source);
  const auto checked = CheckTestSource(parameter.source);
  EXPECT_TRUE(std::ranges::any_of(checked.diagnostics, [&](const Diagnostic& diagnostic) -> bool {
    return diagnostic.message.contains(parameter.message);
  }));
}

INSTANTIATE_TEST_SUITE_P(
    PathsAndReachability,
    ControlFlowErrorTest,
    testing::Values(
        FlowErrorCase{"func f(): int {}", "fall through"},
        FlowErrorCase{"func f(flag: bool): int { if (flag) return 1; }", "fall through"},
        FlowErrorCase{"func f(flag: bool): int { while (flag) { return 1; } }", "fall through"},
        FlowErrorCase{"func f(flag: bool): int { for (;;) { if (flag) break; } }", "fall through"},
        FlowErrorCase{"class A { f(): double { while (true) { break; } } }", "fall through"},
        FlowErrorCase{"func f(): int { return 1; print(2); }", "unreachable"},
        FlowErrorCase{"func f(): void { while (true) { break; print(1); } }", "unreachable"},
        FlowErrorCase{"func f(): void { for (;;) { continue; print(1); } }", "unreachable"},
        FlowErrorCase{"func f(flag: bool): int { if (flag) return 1; else return 2; return 3; }",
                      "unreachable"},
        FlowErrorCase{"func f(): int { for (;;) {} return 1; }", "unreachable"},
        FlowErrorCase{"func f(): int { while (true) { while (true) { break; } } return 1; }",
                      "unreachable"},
        FlowErrorCase{"func f(): int { if (false) return 1; return 2; }", "unreachable"},
        FlowErrorCase{"func f(): void { while (false) {} }", "unreachable"},
        FlowErrorCase{"func f(): void { for (;false;) {} }", "unreachable"},
        FlowErrorCase{"func f(): void { break; }", "inside a loop"},
        FlowErrorCase{"func f(): void { continue; }", "inside a loop"}));

TEST(ControlFlowTest, ForEachDoesNotEstablishFactsFromAPossiblyEmptyBody) {
  const auto checked = CheckTestSource(R"(
class Box {
  value: int;
  constructor(values: int[]) {
    for (const value in values) { this.value = value; }
  }
}
)");
  EXPECT_TRUE(std::ranges::any_of(checked.diagnostics, [](const Diagnostic& diagnostic) {
    return diagnostic.message.contains("must be initialized");
  }));
  EXPECT_FALSE(CheckTestSource(R"(
func read(values: int[]): int {
  let value: int? = null;
  for (const element in values) { value = element; }
  return value;
}
)")
                   .Succeeded());
}

TEST(ControlFlowTest, ForEachConsumesBreaksAndContinuesAndPreservesConstructorExits) {
  const auto checked = CheckTestSource(R"(
class Box {
  value: int;
  constructor(values: int[]) {
    for (const value in values) {
      try { if (value == 0) continue; break; } finally { print(value); }
    }
    this.value = 42;
  }
}
func forever(): int {
  while (true) { for (const value in [1]) { break; } }
}
)");
  for (const auto& diagnostic : checked.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  EXPECT_TRUE(checked.Succeeded());
}

TEST(ControlFlowTest, ForEachRejectsNullableCollectionsAndInvalidatesCapturedFacts) {
  EXPECT_FALSE(CheckTestSource("func f(values: int[]?): void { for (const value in values) {} }")
                   .Succeeded());
  EXPECT_FALSE(CheckTestSource(R"(
func read(values: int[]): int {
  let value: int? = 1;
  if (value == null) return 0;
  for (const item in (((): int[] => { value = null; return values; })())) {}
  return value;
}
)")
                   .Succeeded());
}

class CompleteReturnTest : public testing::TestWithParam<std::string_view> {};

TEST_P(CompleteReturnTest, AcceptsAllFiniteExitsReturningAValue) {
  const auto checked = CheckTestSource(GetParam());
  for (const auto& diagnostic : checked.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  EXPECT_TRUE(checked.Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    BranchesAndLoops,
    CompleteReturnTest,
    testing::Values("func f(flag: bool): int { if (flag) return 1; else return 2; }",
                    "func f(flag: bool): int { if (flag) return 1; return 2; }",
                    "func f(): int { if (true) return 1; }",
                    "func f(): int { while (!false) { return 1; } }",
                    "func f(): int { for (;;) {} }",
                    "func f(flag: bool): int { if (flag) return 1; else for (;;) {} }",
                    "func f(flag: bool): int { while (true) { if (flag) return 1; } }",
                    "func f(): int { for (;;) { while (true) { break; } continue; } }",
                    "func f(): int { while (true) { break; } return 1; }",
                    "func f(flag: bool): void { if (flag) return; }"));

TEST(ControlFlowTest, ReportsTheUnreachableSourceLocation) {
  const auto checked = CheckTestSource("func f(): int {\n  return 1;\n  print(2);\n}");
  ASSERT_EQ(checked.diagnostics.size(), 1U);
  const auto& diagnostic = checked.diagnostics.front();
  EXPECT_EQ(diagnostic.message, "unreachable statement");
  EXPECT_EQ(diagnostic.location.line, 3U);
  EXPECT_EQ(diagnostic.location.column, 3U);
}

TEST(ControlFlowTest, IntersectsConstructorFieldsAcrossBitmapWords) {
  for (const auto omitted : {-1, 0, 63, 64, 127, 128, 129}) {
    SCOPED_TRACE(omitted);
    std::string source = "class Base { inherited: int = 42; } class Box extends Base {";
    for (int field = 0; field < 130; ++field) {
      source += "field" + std::to_string(field) + ": int;";
    }
    source += "static count: int = 0; constructor(flag: bool) { if (flag) {";
    for (int field = 0; field < 130; ++field) {
      source += "this.field" + std::to_string(field) + " = 42;";
    }
    source += "} else {";
    for (int field = 0; field < 130; ++field) {
      if (field != omitted) {
        source += "this.field" + std::to_string(field) + " = 42;";
      }
    }
    source += "} } }";
    const auto checked = CheckTestSource(source);
    if (omitted < 0) {
      for (const auto& diagnostic : checked.diagnostics) {
        ADD_FAILURE() << diagnostic.message;
      }
      EXPECT_TRUE(checked.Succeeded());
    } else {
      ASSERT_EQ(checked.diagnostics.size(), 1U);
      EXPECT_TRUE(checked.diagnostics.front().message.contains(
          "field 'field" + std::to_string(omitted) + "' must be initialized"));
    }
  }
}

}  // namespace
}  // namespace vela::frontend
