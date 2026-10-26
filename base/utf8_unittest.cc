#include "base/utf8.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

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

}  // namespace
}  // namespace vela
