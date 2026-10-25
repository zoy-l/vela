#include <array>
#include <cstdint>
#include <limits>

#include "gtest/gtest.h"

#include "runtime/random.h"

namespace vela::runtime {
namespace {

constexpr std::uint64_t kSeed = 123;

TEST(RandomTest, MatchesXoroshiro128PlusPlusReferenceSequenceAfterSplitMixSeeding) {
  Random random(kSeed);
  constexpr auto expected = std::to_array<std::uint64_t>({
      0x66594270542d9ee8,
      0x9b7b6288acb53cb8,
      0x6fbf0435372ec618,
      0x3d5644414c4b1bc4,
      0xc4eec1f605c9e5fc,
  });
  for (const auto value : expected) {
    EXPECT_EQ(random.Next(), value);
  }
  random.Seed(kSeed);
  EXPECT_EQ(random.Next(), expected.front());
}

TEST(RandomTest, RejectsNonPositiveBounds) {
  Random random;
  EXPECT_FALSE(random.NextInt(0));
  EXPECT_FALSE(random.NextInt(-1));
}

class BoundedRandomTest : public testing::TestWithParam<std::int64_t> {};

TEST_P(BoundedRandomTest, SamplesStayInHalfOpenRange) {
  Random random;
  const auto bound = GetParam();
  constexpr int kIterations = 1000;
  for (int index = 0; index < kIterations; ++index) {
    const auto value = random.NextInt(bound);
    EXPECT_TRUE(value && *value >= 0 && *value < bound);
  }
}

INSTANTIATE_TEST_SUITE_P(Bounds,
                         BoundedRandomTest,
                         testing::Values(std::int64_t{1},
                                         std::int64_t{7},
                                         std::numeric_limits<std::int64_t>::max()));

TEST(RandomTest, RepeatedSeedsAreDeterministicAndDoubleUsesHalfOpenUnitInterval) {
  Random first(kSeed);
  Random second(kSeed);
  constexpr int kIterations = 1000;
  for (int index = 0; index < kIterations; ++index) {
    const auto value = first.NextDouble();
    EXPECT_EQ(value, second.NextDouble());
    EXPECT_GE(value, 0.0);
    EXPECT_LT(value, 1.0);
  }
}

}  // namespace
}  // namespace vela::runtime
