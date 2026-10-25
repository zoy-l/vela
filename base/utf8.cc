#include "base/utf8.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vela {

namespace {
constexpr std::uint32_t kTwoByteFirst = 0xC2;
constexpr std::uint32_t kTwoByteLast = 0xDF;
constexpr std::uint32_t kTwoBytePayload = 0x1F;
constexpr std::uint32_t kContinuationTag = 0x80;
constexpr std::uint32_t kThreeByteTag = 0xE0;
constexpr std::uint32_t kThreeByteLast = 0xEF;
constexpr std::uint32_t kThreeBytePayload = 0x0F;
constexpr std::uint32_t kThreeByteMinimum = 0x800;
constexpr std::uint32_t kFourByteTag = 0xF0;
constexpr std::uint32_t kFourByteLast = 0xF4;
constexpr std::uint32_t kFourBytePayload = 0x07;
constexpr std::uint32_t kContinuationMask = 0xC0;
constexpr std::uint32_t kContinuationPayload = 0x3F;
constexpr unsigned kContinuationBits = 6;
}  // namespace

std::optional<std::size_t> InvalidUtf8Offset(std::string_view text) noexcept {
  for (std::size_t index = 0; index < text.size();) {
    const auto start = index;
    const auto first = static_cast<unsigned char>(text.substr(index++).front());
    std::uint32_t point = first;
    std::size_t remaining = 0;
    std::uint32_t minimum = 0;
    if (first >= kTwoByteFirst && first <= kTwoByteLast) {
      point = first & kTwoBytePayload;
      remaining = 1;
      minimum = kContinuationTag;
    } else if (first >= kThreeByteTag && first <= kThreeByteLast) {
      point = first & kThreeBytePayload;
      remaining = 2;
      minimum = kThreeByteMinimum;
    } else if (first >= kFourByteTag && first <= kFourByteLast) {
      point = first & kFourBytePayload;
      remaining = 3;
      minimum = kSupplementaryFirst;
    } else if (first >= kContinuationTag) {
      return start;
    }
    if (remaining > text.size() - index) {
      return start;
    }
    for (std::size_t count = 0; count < remaining; ++count) {
      const auto next = static_cast<unsigned char>(text.substr(index++).front());
      if ((next & kContinuationMask) != kContinuationTag) {
        return index - 1;
      }
      point = (point << kContinuationBits) | (next & kContinuationPayload);
    }
    if (point < minimum || point > kUnicodeMaximum ||
        (point >= kHighSurrogateFirst && point <= kLowSurrogateLast)) {
      return start;
    }
  }
  return std::nullopt;
}

void AppendUtf8(std::uint32_t point, std::string& text) {
  if (point < kContinuationTag) {
    text.push_back(static_cast<char>(point));
  } else if (point < kThreeByteMinimum) {
    text.push_back(static_cast<char>(kContinuationMask | (point >> kContinuationBits)));
    text.push_back(static_cast<char>(kContinuationTag | (point & kContinuationPayload)));
  } else if (point < kSupplementaryFirst) {
    text.push_back(static_cast<char>(kThreeByteTag | (point >> (2 * kContinuationBits))));
    text.push_back(static_cast<char>(kContinuationTag |
                                     ((point >> kContinuationBits) & kContinuationPayload)));
    text.push_back(static_cast<char>(kContinuationTag | (point & kContinuationPayload)));
  } else {
    text.push_back(static_cast<char>(kFourByteTag | (point >> (3 * kContinuationBits))));
    text.push_back(static_cast<char>(kContinuationTag |
                                     ((point >> (2 * kContinuationBits)) & kContinuationPayload)));
    text.push_back(static_cast<char>(kContinuationTag |
                                     ((point >> kContinuationBits) & kContinuationPayload)));
    text.push_back(static_cast<char>(kContinuationTag | (point & kContinuationPayload)));
  }
}

std::size_t Utf8Length(std::string_view text) noexcept {
  std::size_t length = 0;
  for (const auto byte : text) {
    length += static_cast<std::size_t>((static_cast<unsigned char>(byte) & kContinuationMask) !=
                                       kContinuationTag);
  }
  return length;
}

std::optional<std::size_t> Utf8Offset(std::string_view text, std::size_t index) noexcept {
  std::size_t offset = 0;
  while (index != 0 && offset < text.size()) {
    const auto first = static_cast<unsigned char>(text.substr(offset).front());
    if (first < kContinuationTag) {
      ++offset;
    } else if (first < kThreeByteTag) {
      offset += 2;
    } else if (first < kFourByteTag) {
      offset += 3;
    } else {
      offset += 4;
    }
    --index;
  }
  return index == 0 ? std::optional(offset) : std::nullopt;
}

}  // namespace vela
