#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vela {

inline constexpr std::uint32_t kUnicodeMaximum = 0x10FFFF;
inline constexpr std::uint32_t kHighSurrogateFirst = 0xD800;
inline constexpr std::uint32_t kHighSurrogateLast = 0xDBFF;
inline constexpr std::uint32_t kLowSurrogateFirst = 0xDC00;
inline constexpr std::uint32_t kLowSurrogateLast = 0xDFFF;
inline constexpr std::uint32_t kSupplementaryFirst = 0x10000;
inline constexpr unsigned kSurrogatePayloadBits = 10;
inline constexpr std::uint32_t kContinuationTag = 0x80;
inline constexpr std::uint32_t kContinuationMask = 0xC0;

[[nodiscard]] constexpr bool IsContinuationByte(char byte) noexcept {
  return (static_cast<std::uint8_t>(byte) & kContinuationMask) == kContinuationTag;
}

// 输入边界已保证合法 UTF-8；只分类首字节，不解码后续载荷。
[[nodiscard]] constexpr std::uint32_t Utf8SequenceWidth(char byte) noexcept {
  constexpr std::uint8_t kTwoByteLead = 0xc0;
  constexpr std::uint8_t kThreeByteLead = 0xe0;
  constexpr std::uint8_t kFourByteLead = 0xf0;
  const auto first = static_cast<std::uint8_t>(byte);
  return 1U + static_cast<std::uint32_t>(first >= kTwoByteLead) +
         static_cast<std::uint32_t>(first >= kThreeByteLead) +
         static_cast<std::uint32_t>(first >= kFourByteLead);
}

struct Utf8Point final {
  std::uint32_t value;
  std::size_t width;
};

// 内部文本已通过输入边界验证；Decode 输入非空，Encode 输出至少有编码宽度个字节。
[[nodiscard]] Utf8Point DecodeUtf8Prefix(std::string_view text) noexcept;
[[nodiscard]] constexpr std::size_t Utf8EncodedWidth(std::uint32_t point) noexcept {
  constexpr std::uint32_t kAsciiLimit = 0x80;
  constexpr std::uint32_t kTwoByteLimit = 0x800;
  if (point < kAsciiLimit) {
    return 1;
  }
  if (point < kTwoByteLimit) {
    return 2;
  }
  return point < kSupplementaryFirst ? 3U : 4U;
}
[[nodiscard]] std::size_t EncodeUtf8(std::uint32_t point, std::span<char> output) noexcept;

// 输入边界验证返回首个非法字节的位置；内部字符串操作依赖合法 UTF-8 契约。
// 单点读取只验证首个编码，错误位置相对输入；空输入返回位置 0。
[[nodiscard]] std::expected<Utf8Point, std::size_t> ReadUtf8Prefix(std::string_view text) noexcept;
[[nodiscard]] std::optional<std::size_t> InvalidUtf8Offset(std::string_view text) noexcept;
void AppendUtf8(std::uint32_t point, std::string& text);
[[nodiscard]] std::size_t Utf8Length(std::string_view text) noexcept;
[[nodiscard]] std::optional<std::size_t> Utf8Offset(std::string_view text,
                                                    std::size_t index) noexcept;

}  // namespace vela
