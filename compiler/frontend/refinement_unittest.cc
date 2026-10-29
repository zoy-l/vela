#include <algorithm>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/frontend/checker_test_support.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {
namespace {

TEST(RefinementTest, ForIncrementRejectsFactsInvalidatedByBodyAndContinue) {
  const auto checked = CheckTestSource(R"(
func inspect(value: string?): void {
  for (; value != null; value.length) {
    value = null;
    continue;
  }
}
)");
  EXPECT_FALSE(checked.Succeeded());
  EXPECT_TRUE(std::ranges::any_of(checked.diagnostics, [](const Diagnostic& diagnostic) {
    return diagnostic.message.contains("nullable");
  }));
  EXPECT_TRUE(CheckTestSource(R"(
func inspect(value: string?, skip: bool): void {
  for (; value != null; value.length) {
    if (skip) continue;
    break;
  }
}
)")
                  .Succeeded());
}

TEST(RefinementTest, MatchRestoresEachArmAndIntersectsWrittenFacts) {
  EXPECT_TRUE(CheckTestSource(R"(
func choose(selector: bool, value: string?, stable: string?): int {
  if (stable == null) return 0;
  const selected = match (selector) {
    true => value = 'left',
    false => value = 'right',
  };
  return value.length + stable.length;
}
)")
                  .Succeeded());
  EXPECT_FALSE(CheckTestSource(R"(
func choose(selector: bool, value: string?): int {
  if (value == null) return 0;
  const selected = match (selector) {
    true => value = null,
    false => value,
  };
  return value.length;
}
)")
                   .Succeeded());
}

TEST(RefinementTest, MatchDoesNotLeakEarlierArmWritesIntoLaterArms) {
  const auto checked = CheckTestSource(R"(
func choose(selector: bool, value: string?): string? {
  if (value == null) return null;
  return match (selector) {
    true => value = null,
    false => value + '',
  };
}
)");
  for (const auto& diagnostic : checked.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  EXPECT_TRUE(checked.Succeeded());
}

TEST(RefinementTest, NestedSwitchScopesAndLambdaChecksPreserveOuterBranches) {
  const auto checked = CheckTestSource(R"(
func inspect(selector: int, flag: bool, value: string?): int {
  if (value == null) return 0;
  switch (selector) {
    case 0: {
      if (flag) { const value: int? = null; }
      const identity = (item: int): int => item;
      return identity(value.length);
    }
    case 1: {
      try { if (flag) return value.length; }
      catch { if (value == null) return 0; }
      break;
    }
    default: break;
  }
  return value.length;
}
)");
  for (const auto& diagnostic : checked.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  EXPECT_TRUE(checked.Succeeded());
}

TEST(RefinementTest, ConditionalArmsUseIndependentFactsAndMergeTheirWrites) {
  EXPECT_TRUE(CheckTestSource(R"(
func length(value: string?): int { return value != null ? value.length : 0; }
func choose(flag: bool, value: string?): int {
  const selected = flag ? (value = 'left') : (value = 'right');
  return value.length;
}
)")
                  .Succeeded());
  EXPECT_FALSE(CheckTestSource(R"(
func choose(flag: bool, value: string?): int {
  const selected = flag ? (value = null) : (value = 'right');
  return value.length;
}
)")
                   .Succeeded());
}

TEST(RefinementTest, NestedShortCircuitAndBranchesDoNotRestoreInvalidatedCapturedFacts) {
  EXPECT_FALSE(CheckTestSource(R"(
func inspect(flag: bool): int {
  let value: string? = 'ready';
  const clear = (): bool => { value = null; return true; };
  if (value == null) return 0;
  if (flag) {
    const selected = match (flag) { true => clear(), false => false, };
  } else {
    value = 'other';
  }
  return value.length;
}
)")
                   .Succeeded());
}

}  // namespace
}  // namespace vela::frontend
