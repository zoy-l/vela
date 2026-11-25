#include "benchmarks/standard_api_workloads.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/utf8.h"

namespace vela::benchmarks {
namespace {

constexpr double kChecksumScale = 1000.0;
constexpr std::string_view kUnit("A中😀éZ\0", sizeof("A中😀éZ\0") - 1);
constexpr std::size_t kTextUnits = 4;
constexpr std::int64_t kPositivePeriod = 97;
constexpr std::int64_t kSignedPeriod = 128;
constexpr std::int64_t kSignedCenter = kSignedPeriod / 2;
constexpr std::int64_t kWidePeriod = 257;
constexpr std::int64_t kWideCenter = kWidePeriod / 2;
constexpr std::int64_t kDivisorPeriod = 7;

const std::string kText = [] {
  std::string text;
  for (std::size_t index = 0; index < kTextUnits; ++index) {
    text.append(kUnit);
  }
  return text;
}();
const std::size_t kTextPoints = Utf8Length(kText);
const std::array<std::string, 3> kTrimInputs{kText, " \u3000" + kText + "\u0085\t", " \u3000\t"};

bool IsBoundary(std::string_view text, std::int64_t offset) {
  return offset >= 0 && std::cmp_less_equal(offset, text.size()) &&
         (std::cmp_equal(offset, text.size()) ||
          !IsContinuationByte(text[static_cast<std::size_t>(offset)]));
}

bool IsWhitespace(std::uint32_t point) {
  // C++ 对照按 Unicode White_Space 判定，使用与 API 独立的属性表达。
  constexpr auto kOtherWhitespace = std::to_array<std::uint32_t>(
      {0x20, 0x85, 0xa0, 0x1680, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000});
  if ((point >= U'\t' && point <= U'\r') || (point >= U'\u2000' && point <= U'\u200a')) {
    return true;
  }
  return std::ranges::any_of(kOtherWhitespace,
                             [point](auto candidate) { return point == candidate; });
}

std::string_view Trim(std::string_view text) {
  while (!text.empty()) {
    const auto point = DecodeUtf8Prefix(text);
    if (!IsWhitespace(point.value)) {
      break;
    }
    text.remove_prefix(point.width);
  }
  while (!text.empty()) {
    auto start = text.size() - 1;
    while (IsContinuationByte(text[start])) {
      --start;
    }
    if (!IsWhitespace(DecodeUtf8Prefix(text.substr(start)).value)) {
      break;
    }
    text.remove_suffix(text.size() - start);
  }
  return text;
}

}  // namespace

std::int64_t MathAtan2(std::span<const std::int64_t> input) {
  std::int64_t checksum = 0;
  for (const auto value : input) {
    checksum += static_cast<std::int64_t>(
        std::round(std::atan2(static_cast<double>((value % kPositivePeriod) + 1),
                              static_cast<double>((value % kSignedPeriod) - kSignedCenter)) *
                   kChecksumScale));
  }
  return checksum;
}

std::int64_t MathCbrt(std::span<const std::int64_t> input) {
  std::int64_t checksum = 0;
  for (const auto value : input) {
    checksum += static_cast<std::int64_t>(std::round(
        std::cbrt(static_cast<double>((value % kWidePeriod) - kWideCenter)) * kChecksumScale));
  }
  return checksum;
}

std::int64_t MathHypot(std::span<const std::int64_t> input) {
  std::int64_t checksum = 0;
  for (const auto value : input) {
    checksum += static_cast<std::int64_t>(
        std::round(std::hypot(static_cast<double>((value % kSignedPeriod) - kSignedCenter),
                              static_cast<double>((value % kPositivePeriod) + 1)) *
                   kChecksumScale));
  }
  return checksum;
}

std::int64_t MathRemainder(std::span<const std::int64_t> input) {
  std::int64_t checksum = 0;
  for (const auto value : input) {
    checksum += static_cast<std::int64_t>(
        std::round(std::remainder(static_cast<double>((value % kWidePeriod) - kWideCenter),
                                  static_cast<double>((value % kDivisorPeriod) + 1)) *
                   kChecksumScale));
  }
  return checksum;
}

std::int64_t StringBoundary(std::span<const std::int64_t> input) {
  std::int64_t checksum = 0;
  const auto limit = static_cast<std::int64_t>(kUnit.size()) + 2;
  for (const auto value : input) {
    checksum += IsBoundary(kUnit, (value % limit) - 1) ? 1 : 0;
  }
  return checksum;
}

std::int64_t StringSliceBytes(std::span<const std::int64_t> input) {
  std::int64_t checksum = 0;
  for (const auto value : input) {
    const auto start = ((value % 2) * static_cast<std::int64_t>(kUnit.size())) + 1;
    const auto end = start + static_cast<std::int64_t>(2 * kUnit.size());
    if (!IsBoundary(kText, start) || !IsBoundary(kText, end)) {
      return -1;
    }
    const std::string result(kText, static_cast<std::size_t>(start),
                             static_cast<std::size_t>(end - start));
    if (!result.starts_with("中") || !result.ends_with('A')) {
      return -1;
    }
    // std::string 不缓存码点数；在创建结果时计算同一份长度元数据。
    checksum += static_cast<std::int64_t>(result.size() + Utf8Length(result));
  }
  return checksum;
}

std::int64_t StringReplaceBytes(std::span<const std::int64_t> input) {
  std::int64_t checksum = 0;
  for (const auto value : input) {
    const auto start = ((value % static_cast<std::int64_t>(kTextUnits)) *
                        static_cast<std::int64_t>(kUnit.size())) +
                       1;
    const auto end = start + static_cast<std::int64_t>(std::string_view("中").size());
    if (!IsBoundary(kText, start) || !IsBoundary(kText, end)) {
      return -1;
    }
    const std::string_view inserted = value % 2 == 0 ? "好" : "🙂";
    const auto first = static_cast<std::size_t>(start);
    const auto last = static_cast<std::size_t>(end);
    std::string result;
    result.reserve(kText.size() - (last - first) + inserted.size());
    result.append(kText, 0, first);
    result.append(inserted);
    result.append(kText, last, std::string::npos);
    checksum += static_cast<std::int64_t>(result.size() + Utf8Length(result));
  }
  return checksum;
}

std::int64_t StringByteOffset(std::span<const std::int64_t> input) {
  std::int64_t checksum = 0;
  std::size_t index = 0;
  std::size_t offset = 0;
  for (const auto value : input) {
    const auto target = (value % static_cast<std::int64_t>(kTextPoints + 2)) - 1;
    if (target < 0 || std::cmp_greater(target, kTextPoints)) {
      --checksum;
      continue;
    }
    // 与 Vela 相同，从缓存位置或最近端点开始，不把每次从头扫描作为对照。
    const auto requested = static_cast<std::size_t>(target);
    const auto distance = [](std::size_t first, std::size_t second) {
      return first > second ? first - second : second - first;
    };
    if (requested < distance(requested, index)) {
      index = 0;
      offset = 0;
    }
    if (kTextPoints - requested < distance(requested, index)) {
      index = kTextPoints;
      offset = kText.size();
    }
    while (index < requested) {
      offset += DecodeUtf8Prefix(std::string_view(kText).substr(offset)).width;
      ++index;
    }
    while (index > requested) {
      --offset;
      while (IsContinuationByte(kText[offset])) {
        --offset;
      }
      --index;
    }
    checksum += static_cast<std::int64_t>(offset);
  }
  return checksum;
}

std::int64_t StringTrim(std::span<const std::int64_t> input) {
  std::int64_t checksum = 0;
  for (const auto value : input) {
    const auto& text = kTrimInputs[static_cast<std::size_t>(value) % kTrimInputs.size()];
    const auto trimmed = Trim(text);
    if (trimmed.size() == text.size()) {
      checksum += static_cast<std::int64_t>(text.size() + kTextPoints);
    } else {
      const std::string result(trimmed);
      checksum += static_cast<std::int64_t>(result.size() + Utf8Length(result));
    }
  }
  return checksum;
}

}  // namespace vela::benchmarks
