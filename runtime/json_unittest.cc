#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "base/execution_status.h"
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
  EXPECT_EQ(value.error().status, ExecutionStatus::JsonDecodeError);
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
  ASSERT_TRUE(result->IsString());
  EXPECT_EQ(result->AsString()->value(), "{\"a\":2,\"\\u0000\":\"😀\"}");
}

TEST(JsonRuntimeTest, NumericErrorUsesRawTokenOffsetAfterEscapedAndUnicodeStrings) {
  Isolate isolate(1);
  constexpr std::string_view kSource =
      R"({"escaped":"\u0031\"2","items":[3,{"😀":4,"overflow":1e9999}]})";
  const auto parsed = ParseJson(kSource, isolate);
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().status, ExecutionStatus::JsonDecodeError);
  EXPECT_EQ(parsed.error().offset, kSource.find("1e9999"));
}

TEST(JsonRuntimeTest, QuotesAllControlBytesAndUnicodeWithoutLosingContent) {
  Isolate isolate(1);
  std::string source;
  constexpr int kFirstPrintableByte = 32;
  for (int byte = 0; byte < kFirstPrintableByte; ++byte) {
    source += static_cast<char>(byte);
  }
  source += "\"\\中😀";
  auto& heap = isolate.heap();
  JsonEncoder encoder(isolate, Value::String(heap.AllocateString(source)));
  const auto encoded = encoder.Advance();
  ASSERT_TRUE(encoded);
  RootRange roots(heap, &*encoded, 1);
  const auto decoded = ParseJson(encoded->AsString()->value(), isolate);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->AsString()->value(), source);
}

TEST(JsonRuntimeTest, ReusesTraversalFramesAcrossSiblingsWithDifferentContainerKinds) {
  Isolate isolate(1);
  constexpr std::string_view kSource = R"([[1],{"a":[2,3]},[],{"b":{}},[4]])";
  const auto parsed = ParseJson(kSource, isolate);
  ASSERT_TRUE(parsed);
  JsonEncoder encoder(isolate, *parsed);
  const auto encoded = encoder.Advance();
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded->AsString()->value(), kSource);
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
    EXPECT_EQ(result->AsString()->value(), source);
  }
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().allocated_bytes(), baseline);
}

TEST(JsonRuntimeTest, RootsPendingInputBeforeAdvancingAndReleasesItOnDestruction) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  {
    auto* input = heap.AllocateString("pending 😀");
    ASSERT_NE(input, nullptr);
    JsonEncoder encoder(isolate, Value::String(input));
    heap.Collect();
    const auto result = encoder.Advance();
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->IsString());
    EXPECT_EQ(result->AsString()->value(), "\"pending 😀\"");
  }
  heap.FinishCollection();
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), 0U);
}

}  // namespace
}  // namespace vela::runtime
