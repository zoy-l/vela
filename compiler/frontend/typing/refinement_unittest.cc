#include <algorithm>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/typing/checker_test_support.h"

namespace vela::frontend {
namespace {

TEST(RefinementTest, NonNullInitializersEstablishFactsWithoutChangingDeclaredTypes) {
  const auto checked = CheckTestSource(R"(
func make(): int { return 3; }
func bounded<T extends int>(input: T): int { let value: int? = input; return value + 1; }
func main(): int {
  let value: int? = make();
  const text: string? = 'ready';
  const operation: (() => int)? = (): int => 1;
  const values: int[]? = [2];
  const retained: int = value ?? 0;
  { let value: int? = null; }
  const result = value + text.length + operation() + values[0] + retained;
  value = null;
  value = 4;
  return result + value;
}
)");
  for (const auto& diagnostic : checked.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  EXPECT_TRUE(checked.Succeeded());
}

class NullableOperandDiagnosticTest : public testing::TestWithParam<std::string_view> {};

TEST_P(NullableOperandDiagnosticTest, ReportsTheNullableOperandBeforeNumericTypeMismatches) {
  const auto source = "func inspect(x: int?): void { " + std::string(GetParam()) + "; }";
  const auto checked = CheckTestSource(source);
  EXPECT_FALSE(checked.Succeeded());
  ASSERT_EQ(checked.diagnostics.size(), 1U);
  const auto& diagnostic = checked.diagnostics.front();
  EXPECT_TRUE(diagnostic.message.contains("'x' may be null")) << diagnostic.message;
  EXPECT_EQ(diagnostic.location.line, 1U);
  EXPECT_EQ(diagnostic.location.column, source.rfind('x') + 1);
}

INSTANTIATE_TEST_SUITE_P(Operators,
                         NullableOperandDiagnosticTest,
                         testing::Values("x + 1",
                                         "1 + x",
                                         "x - 1",
                                         "x * 1",
                                         "x / 1",
                                         "x < 1",
                                         "x & 1",
                                         "x << 1",
                                         "-x",
                                         "~x",
                                         "x++",
                                         "x += 1",
                                         "x &= 1",
                                         "x %= 1"));

TEST(RefinementTest, NullableInitializersAndLaterEffectsCannotEstablishNonNullFacts) {
  for (const auto* const source : {
           "func inspect(value:int?):int{let x:int?=value;return x+1;}",
           "func inspect(value:dynamic):int{let x:int?=value;return x+1;}",
           "func inspect(value:dynamic):int{let x:int?=3;x=value;return x+1;}",
           "func inspect():int{let x:int?=3;x=null;return x+1;}",
           "func inspect(flag:bool):int{let x:int?=3;while(flag){x=null;}return x+1;}",
           "func inspect():int{let x:int?=3;const clear=():void=>{x=null;};clear();return x+1;}",
           "func inspect<T extends int?>(value:T):int{let x:int?=value;return x+1;}",
       }) {
    SCOPED_TRACE(source);
    const auto checked = CheckTestSource(source);
    EXPECT_FALSE(checked.Succeeded());
    EXPECT_TRUE(std::ranges::any_of(checked.diagnostics, [](const Diagnostic& diagnostic) {
      return diagnostic.message.contains("'x' may be null");
    }));
  }
}

TEST(RefinementTest, NullFormattingAndNumericMismatchDiagnosticsKeepTheirContracts) {
  EXPECT_TRUE(CheckTestSource("func format(x:int?):string{return 'value='+x;}").Succeeded());
  const auto checked = CheckTestSource("func invalid():double{return 1+2.0;}");
  EXPECT_TRUE(std::ranges::any_of(checked.diagnostics, [](const Diagnostic& diagnostic) {
    return diagnostic.message.contains("arithmetic operands must have the same numeric type");
  }));
}

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
