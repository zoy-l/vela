#include "base/utf8.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"

namespace vela {
namespace {

TEST(Utf8Test, EncodesKnownScalarBoundaryVectors) {
  struct Sample {
    std::uint32_t point;
    std::string_view bytes;
  };
  constexpr auto samples = std::to_array<Sample>({
      {.point = 0, .bytes = std::string_view("\0", 1)},
      {.point = 0x41, .bytes = "A"},
      {.point = 0xa2, .bytes = "¢"},
      {.point = 0x20ac, .bytes = "€"},
      {.point = 0x10400, .bytes = "𐐀"},
      {.point = 0x10ffff, .bytes = "\xf4\x8f\xbf\xbf"},
  });
  for (const auto& sample : samples) {
    std::string text;
    AppendUtf8(sample.point, text);
    EXPECT_EQ(text, sample.bytes);
    const auto decoded = DecodeUtf8Prefix(text);
    EXPECT_EQ(decoded.value, sample.point);
    EXPECT_EQ(decoded.width, sample.bytes.size());
  }
}

TEST(Utf8Test, AllUnicodeScalarsProduceValidatedUtf8AndRoundTrip) {
  std::array<char, 4> buffer{};
  for (std::uint32_t point = 0; point <= kUnicodeMaximum; ++point) {
    if (point >= kHighSurrogateFirst && point <= kLowSurrogateLast) {
      continue;
    }
    const auto width = EncodeUtf8(point, buffer);
    const std::string_view text(buffer.data(), width);
    ASSERT_FALSE(InvalidUtf8Offset(text)) << point;
    const auto decoded = DecodeUtf8Prefix(text);
    EXPECT_EQ(decoded.value, point);
    EXPECT_EQ(decoded.width, width);
  }
}

TEST(Utf8Test, PrefixValidationKeepsByteOffsetsAndStopsBeforeTheNextPoint) {
  constexpr auto kInvalid = std::to_array<std::pair<std::string_view, std::size_t>>({
      {"", 0},
      {"\xc0\x80", 0},
      {"\xed\xa0\x80", 0},
      {"\xf4\x90\x80\x80", 0},
      {"\xe2\x82", 0},
      {"\xe2!\x80", 1},
      {"\xe2\x82!", 2},
  });
  for (const auto& [text, offset] : kInvalid) {
    SCOPED_TRACE(offset);
    const auto prefix = ReadUtf8Prefix(text);
    ASSERT_FALSE(prefix);
    EXPECT_EQ(prefix.error(), offset);
    if (!text.empty()) {
      EXPECT_EQ(InvalidUtf8Offset(std::string("ok") + std::string(text)), offset + 2);
    }
  }
  const auto prefix = ReadUtf8Prefix("\xc2\xa2\xff");
  ASSERT_TRUE(prefix);
  EXPECT_EQ(prefix->value, 0xa2U);
  EXPECT_EQ(prefix->width, 2U);
  const auto empty = InvalidUtf8Offset("");
  EXPECT_FALSE(empty);
}

}  // namespace
}  // namespace vela
