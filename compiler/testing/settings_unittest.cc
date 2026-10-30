#include "compiler/testing/settings.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace vela::testing {
namespace {

TEST(TestSettingsTest, NormalizesDurationFactorsAndNoneWithoutChangingInheritance) {
  const auto duration = ParseTimeout("1m 30s");
  ASSERT_TRUE(duration);
  EXPECT_EQ(duration->duration, std::chrono::milliseconds(90000));
  const auto factor = ParseTimeout("2x");
  ASSERT_TRUE(factor);
  EXPECT_EQ(*factor->Apply(kDefaultTimeout), std::chrono::milliseconds(60000));
  EXPECT_EQ(*factor->Apply(std::chrono::milliseconds(7000)), std::chrono::milliseconds(14000));
  EXPECT_EQ(*factor->Apply(std::nullopt), std::nullopt);
  const auto none = ParseTimeout("none");
  ASSERT_TRUE(none);
  EXPECT_EQ(*none->Apply(kDefaultTimeout), std::nullopt);
  ASSERT_TRUE(ParseTimeout("0ms"));
  EXPECT_EQ(ParseTimeout("0ms")->duration.count(), 0);
}

TEST(TestSettingsTest, RejectsMalformedOrUnrepresentableTimeouts) {
  for (const auto* text : {
           "",
           "-1s",
           "1",
           "1w",
           "NaNs",
           "Infinityx",
           "0x",
           "-2x",
           "1s2s",
           "2x 1s",
           "100000000000000x",
           "100000000000000ms",
           "999999999999999999999999999999999999999999999999d",
       }) {
    EXPECT_FALSE(ParseTimeout(text)) << text;
  }
  const auto factor = ParseTimeout("1000000x");
  ASSERT_TRUE(factor);
  EXPECT_FALSE(factor->Apply(std::chrono::milliseconds(1000000000)));
}

TEST(TestSettingsTest, EvaluatesTagPrecedenceParenthesesAndRepeatedNegation) {
  const std::vector<std::string> tags{"unit", "slow"};
  for (const auto* text : {
           "unit",
           "unit || network && missing",
           "unit && (slow || network)",
           "!!unit",
           "!(network || missing) && slow",
       }) {
    const auto selector = TagSelector::Parse(text);
    ASSERT_TRUE(selector) << text;
    EXPECT_TRUE(selector->Matches(tags)) << text;
  }
  for (const auto* text : {"!unit", "(unit || network) && missing", "unit && !slow"}) {
    const auto selector = TagSelector::Parse(text);
    ASSERT_TRUE(selector) << text;
    EXPECT_FALSE(selector->Matches(tags)) << text;
  }
}

TEST(TestSettingsTest, RejectsInvalidTagSelectors) {
  for (const auto* text : {
           "",
           "!",
           "()",
           "unit slow",
           "unit & slow",
           "unit ||",
           "(unit",
           "unit)",
           "unit( slow)",
           "unit && || slow",
           "unit !slow",
       }) {
    EXPECT_FALSE(TagSelector::Parse(text)) << text;
  }
}

}  // namespace
}  // namespace vela::testing
