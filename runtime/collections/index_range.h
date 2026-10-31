#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <utility>

namespace vela::runtime {

// 连续容器的长度不超过 int64 上界；负索引从末尾计数，再夹到有效边界。
template <std::unsigned_integral Size>
constexpr Size RelativeIndex(std::int64_t index, Size length) noexcept {
  const auto size = static_cast<std::int64_t>(length);
  return static_cast<Size>(index < 0 ? std::max(size + index, std::int64_t{0})
                                     : std::min(index, size));
}

template <std::unsigned_integral Size>
constexpr std::pair<Size, Size> ClampIndexRange(std::int64_t first,
                                                std::int64_t last,
                                                Size length) noexcept {
  const auto start = RelativeIndex(first, length);
  return {start, std::max(start, RelativeIndex(last, length))};
}

}  // namespace vela::runtime
