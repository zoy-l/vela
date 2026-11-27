#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <yyjson.h>

#include "gtest/gtest.h"

#include "base/json.h"

namespace vela {
namespace {

TEST(JsonDocumentTest, OwnsDecodedStringsAcrossInputDestructionAndMoves) {
  auto parsed = [] {
    const std::string input = R"({"text":"Vela \u4e2d\u6587"})";
    return JsonDocument::Parse(std::as_bytes(std::span(input)));
  }();
  ASSERT_TRUE(parsed.has_value());
  const auto moved = std::move(*parsed);
  EXPECT_EQ(GetJsonString(moved.root(), "text"), "Vela 中文");
}

TEST(JsonDocumentTest, RejectsMalformedJsonAndReportsItsBytePosition) {
  const auto parsed = JsonDocument::Parse(R"({"value":})");
  ASSERT_FALSE(parsed.has_value());
  EXPECT_GT(parsed.error().offset, 0U);
  EXPECT_FALSE(parsed.error().message.empty());
  EXPECT_FALSE(JsonDocument::Parse(R"({"value":1,})").has_value());
  EXPECT_FALSE(JsonDocument::Parse(R"({"value":NaN})").has_value());
}

TEST(JsonDocumentTest, RawNumbersKeepOriginalOffsetsAfterDocumentMove) {
  constexpr std::string_view kSource = R"({"text":"\u4e2d😀","value":42})";
  auto parsed = JsonDocument::Parse(kSource, YYJSON_READ_NUMBER_AS_RAW);
  ASSERT_TRUE(parsed);
  auto document = std::move(*parsed);
  const auto* number = GetJsonField(document.root(), "value");
  ASSERT_TRUE(yyjson_is_raw(number));
  EXPECT_EQ(document.RawOffset(*number), kSource.find("42"));
}

TEST(JsonOutputTest, CopiesKeysAndEscapedUnicodeTextAcrossMoves) {
  JsonOutput output;
  constexpr std::string_view kOwnedKey = "owned key";
  constexpr std::string_view kText = "你好😀\n\"quoted\"\\";
  {
    const std::string text(kText);
    std::string key(kOwnedKey);
    auto* const document = output.document();
    auto* const value = yyjson_mut_strncpy(document, text.data(), text.size());
    ASSERT_NE(value, nullptr);
    AddJsonField(document, output.root(), key, value);
    key.front() = 'X';
  }
  auto moved = std::move(output);
  const auto encoded = moved.Serialize();
  ASSERT_TRUE(encoded);
  const auto decoded = JsonDocument::Parse(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(GetJsonString(decoded->root(), kOwnedKey), kText);
}

TEST(JsonOutputTest, ReportsInvalidNumbersWithoutMisclassifyingAllocationFailure) {
  JsonOutput output;
  ASSERT_TRUE(yyjson_mut_obj_add_real(output.document(), output.root(), "value",
                                      std::numeric_limits<double>::infinity()));
  const auto encoded = output.Serialize();
  ASSERT_FALSE(encoded);
  EXPECT_FALSE(encoded.error().out_of_memory);
  EXPECT_FALSE(encoded.error().message.empty());
}

}  // namespace
}  // namespace vela
