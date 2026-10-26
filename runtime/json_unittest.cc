#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/json.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

TEST(JsonRuntimeTest, PreservesSigned64BitIntegerEndpoints) {
  Isolate isolate(1);
  const auto minimum = ParseJson("-9223372036854775808", isolate);
  ASSERT_TRUE(minimum);
  EXPECT_EQ(minimum->AsInteger(), std::numeric_limits<std::int64_t>::min());
  const auto maximum = ParseJson("9223372036854775807", isolate);
  ASSERT_TRUE(maximum);
  EXPECT_EQ(maximum->AsInteger(), std::numeric_limits<std::int64_t>::max());
}

class JsonDoubleTest : public testing::TestWithParam<std::string_view> {};
TEST_P(JsonDoubleTest, PreservesFiniteDoubleKindAndSign) {
  Isolate isolate(1);
  const auto value = ParseJson(GetParam(), isolate);
  ASSERT_TRUE(value);
  ASSERT_TRUE(value->IsDouble());
  EXPECT_TRUE(std::isfinite(value->AsDouble()));
  EXPECT_EQ(std::signbit(value->AsDouble()), GetParam().starts_with('-'));
}
INSTANTIATE_TEST_SUITE_P(
    Binary64Boundaries,
    JsonDoubleTest,
    testing::Values("1.0", "1e0", "-0.0", "1e-9999", "-1e-9999", "1.7976931348623157e308"));

class JsonNumberRangeTest : public testing::TestWithParam<std::string_view> {};
TEST_P(JsonNumberRangeTest, RejectsOutOfRangeNumberAtItsOriginalOffset) {
  Isolate isolate;
  const auto value = ParseJson(GetParam(), isolate);
  ASSERT_FALSE(value);
  EXPECT_EQ(value.error().failure, JsonFailure::Decode);
  EXPECT_EQ(value.error().offset, 0U);
}
INSTANTIATE_TEST_SUITE_P(InvalidNumberRanges,
                         JsonNumberRangeTest,
                         testing::Values("9223372036854775808",
                                         "18446744073709551616",
                                         "-9223372036854775809",
                                         "1e9999"));

TEST(JsonRuntimeTest, RejectsInvalidSyntaxAndUnicodeWithOriginalOffsets) {
  Isolate isolate;
  for (const auto* const source : {
           "",
           "true false",
           "[1,]",
           "//comment\nnull",
           "[NaN]",
           "Infinity",
           "\xef\xbb\xbf{}",
           R"("\uD800")",
           R"("\uDC00")",
           R"("\q")",
           "\"\xff\"",
           "\"\n\"",
       }) {
    EXPECT_FALSE(ParseJson(source, isolate)) << source;
  }
  const std::string_view source = R"({"1\"":null,"items":["12",-9223372036854775809]})";
  const auto overflow = ParseJson(source, isolate);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().offset, source.find("-9223"));
  const auto eof = ParseJson("[1,", isolate);
  ASSERT_FALSE(eof);
  EXPECT_EQ(eof.error().offset, 3U);
}

TEST(JsonRuntimeTest, OwnsUnicodeAndEmbeddedNullAndRetainsDuplicateOrder) {
  Isolate isolate(1);
  const auto parsed = ParseJson(R"({"\u0061":1,"\u0000":"\ud83d\ude00","a":2})", isolate);
  ASSERT_TRUE(parsed);
  RootRange roots(isolate.heap(), &*parsed, 1);
  isolate.heap().Collect();
  JsonEncoder encoder(isolate, *parsed);
  const auto result = encoder.Advance();
  ASSERT_TRUE(result);
  ASSERT_EQ(result->action, JsonEncodeAction::Complete);
  EXPECT_EQ(result->value.AsString()->value(), "{\"a\":2,\"\\u0000\":\"😀\"}");
}

TEST(JsonRuntimeTest, TraversesDeepValuesWithoutCppRecursionAndReleasesNativeMemory) {
  Isolate isolate;
  constexpr std::size_t kDepth = 5000;
  const auto source = std::string(kDepth, '[') + "-0.0" + std::string(kDepth, ']');
  const auto baseline = isolate.heap().allocated_bytes();
  {
    const auto value = ParseJson(source, isolate);
    ASSERT_TRUE(value);
    RootRange roots(isolate.heap(), &*value, 1);
    JsonEncoder encoder(isolate, *value);
    const auto result = encoder.Advance();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->value.AsString()->value(), source);
  }
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().allocated_bytes(), baseline);
}

}  // namespace
}  // namespace vela::runtime
