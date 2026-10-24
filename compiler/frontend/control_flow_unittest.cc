#include <algorithm>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/frontend/checker.h"
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
  const auto checked = CheckSource(parameter.source);
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

class CompleteReturnTest : public testing::TestWithParam<std::string_view> {};

TEST_P(CompleteReturnTest, AcceptsAllFiniteExitsReturningAValue) {
  const auto checked = CheckSource(GetParam());
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
  const auto checked = CheckSource("func f(): int {\n  return 1;\n  print(2);\n}");
  ASSERT_EQ(checked.diagnostics.size(), 1U);
  const auto& diagnostic = checked.diagnostics.front();
  EXPECT_EQ(diagnostic.message, "unreachable statement");
  EXPECT_EQ(diagnostic.location.line, 3U);
  EXPECT_EQ(diagnostic.location.column, 3U);
}

}  // namespace
}  // namespace vela::frontend
