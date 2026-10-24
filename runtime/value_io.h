#pragma once

#include <cstddef>
#include <iosfwd>
#include <span>
#include <string_view>

#include "runtime/value.h"

namespace vela::runtime {

// binary64 最短往返格式及 int64 十进制格式均不超过 32 个 ASCII 字节。
inline constexpr std::size_t kScalarTextCapacity = 32;
[[nodiscard]] auto FormatScalar(Value value, std::span<char, kScalarTextCapacity> buffer)
    -> std::string_view;

void WriteValue(std::ostream& output, Value value);

}  // namespace vela::runtime
