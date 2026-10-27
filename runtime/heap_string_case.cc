#include "runtime/heap.h"
#include "runtime/object.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <utf8proc.h>

#include "base/utf8.h"

#include "runtime/string.h"
#include "runtime/unicode_case_data.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

constexpr std::int32_t kCapitalSigma = 0x3a3;
constexpr std::int32_t kFinalSigma = 0x3c2;

std::int32_t NextPoint(std::string_view& remaining) {
  const auto point = DecodeUtf8Prefix(remaining);
  remaining.remove_prefix(point.width);
  return static_cast<std::int32_t>(point.value);
}

bool HasProperty(std::int32_t point, std::span<const internal::CaseRange> ranges) {
  const auto found = std::ranges::lower_bound(ranges, point, {}, &internal::CaseRange::last);
  return found != ranges.end() && found->first <= point;
}

bool IsCased(std::int32_t point) {
  return HasProperty(point, internal::kCasedRanges);
}

bool IsCaseIgnorable(std::int32_t point) {
  return HasProperty(point, internal::kCaseIgnorableRanges);
}

bool HasFollowingCased(std::string_view remaining) {
  while (!remaining.empty()) {
    const auto point = NextPoint(remaining);
    if (!IsCaseIgnorable(point)) {
      return IsCased(point);
    }
  }
  return false;
}

template <typename Visitor>
void WalkCase(std::string_view text, StringCase conversion, const Visitor& visitor) {
  bool preceding_cased = false;
  while (!text.empty()) {
    const auto point = NextPoint(text);
    std::array<std::int32_t, 1> simple{
        conversion == StringCase::Lower ? utf8proc_tolower(point) : utf8proc_toupper(point),
    };
    std::span<const std::int32_t> mapping = simple;
    const auto& mappings = internal::kFullCaseMappings;
    const auto* full = std::ranges::lower_bound(mappings, point, {}, &internal::CaseMapping::point);
    if (full != mappings.end() && full->point == point) {
      mapping = conversion == StringCase::Lower ? std::span(full->lower).first(full->lower_size)
                                                : std::span(full->upper).first(full->upper_size);
    }
    // Σ 自身非 Case_Ignorable，相邻 Σ 的前视区间不重叠，整串仍为线性扫描。
    if (conversion == StringCase::Lower && point == kCapitalSigma && preceding_cased &&
        !HasFollowingCased(text)) {
      simple.front() = kFinalSigma;
      mapping = simple;
    }
    visitor(point, mapping);
    if (conversion == StringCase::Lower && !IsCaseIgnorable(point)) {
      preceding_cased = IsCased(point);
    }
  }
}

}  // namespace

StringObject* Heap::ConvertStringCase(StringObject& source, StringCase conversion) noexcept {
  const auto text = source.value();
  std::optional<std::size_t> length = 0;
  bool changed = false;
  WalkCase(text, conversion, [&](std::int32_t point, std::span<const std::int32_t> mapping) {
    changed = changed || mapping.size() != 1 || mapping.front() != point;
    for (const auto result : mapping) {
      const auto width = Utf8EncodedWidth(static_cast<std::uint32_t>(result));
      if (length && width <= kMaximumStringLength - *length) {
        *length += width;
      } else {
        length.reset();
      }
    }
  });
  if (!length) {
    return nullptr;
  }
  if (!changed) {
    return &source;
  }
  const auto value = Value::String(&source);
  RootRange root(*this, &value, 1);
  auto* output = AllocateStringStorage(*length);
  if (output == nullptr) {
    return nullptr;
  }
  void* storage = StorageAfter<char>(*output);
  auto remaining = std::span(static_cast<char*>(storage), *length);
  WalkCase(text, conversion, [&](std::int32_t, std::span<const std::int32_t> mapping) {
    for (const auto result : mapping) {
      const auto width = EncodeUtf8(static_cast<std::uint32_t>(result), remaining);
      remaining = remaining.subspan(width);
    }
  });
  return output;
}

}  // namespace vela::runtime
