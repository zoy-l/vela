#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <yyjson.h>

#include "gtest/gtest.h"

#include "base/json.h"

namespace vela {
namespace {

TEST(JsonDocumentTest, OwnsDecodedStringsAcrossInputDestructionAndMoves) {
  auto parsed = []() -> std::expected<JsonDocument, JsonError> {
    const std::string input = R"({"text":"Vela \u4e2d\u6587","value":42})";
    return JsonDocument::Parse(input);
  }();
  ASSERT_TRUE(parsed.has_value());
  auto moved = std::move(*parsed);
  const auto* const text = yyjson_obj_get(moved.root(), "text");
  ASSERT_TRUE(yyjson_is_str(text));
  EXPECT_EQ(std::string(yyjson_get_str(text), yyjson_get_len(text)), "Vela 中文");
  EXPECT_EQ(yyjson_get_sint(yyjson_obj_get(moved.root(), "value")), 42);
}

TEST(JsonDocumentTest, RejectsMalformedJsonAndReportsItsBytePosition) {
  const auto parsed = JsonDocument::Parse(R"({"value":})");
  ASSERT_FALSE(parsed.has_value());
  EXPECT_GT(parsed.error().offset, 0U);
  EXPECT_FALSE(parsed.error().message.empty());
  EXPECT_FALSE(JsonDocument::Parse(R"({"value":1,})").has_value());
  EXPECT_FALSE(JsonDocument::Parse(R"({"value":NaN})").has_value());
}

TEST(JsonOutputTest, CopiesEscapedUnicodeTextAndMovesItsOwnedArena) {
  JsonOutput output;
  constexpr std::string_view kOwnedKey = "owned key";
  {
    const std::string text = "你好😀\n\"quoted\"\\";
    ASSERT_TRUE(yyjson_mut_obj_add_strncpy(output.document(), output.root(), "text", text.data(),
                                           text.size()));
    std::string key(kOwnedKey);
    AddJsonField(output.document(), output.root(), key, yyjson_mut_true(output.document()));
    key.front() = 'X';
  }
  auto moved = std::move(output);
  const auto encoded = moved.Serialize();
  ASSERT_TRUE(encoded);
  const auto decoded = JsonDocument::Parse(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(yyjson_is_true(GetJsonField(decoded->root(), kOwnedKey)));
  EXPECT_STREQ(yyjson_get_str(yyjson_obj_get(decoded->root(), "text")), "你好😀\n\"quoted\"\\");
}

}  // namespace
}  // namespace vela
