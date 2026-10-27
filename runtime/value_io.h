#pragma once

#include <cstddef>
#include <iosfwd>
#include <span>
#include <string_view>

#include "runtime/value.h"

namespace vela::runtime {

// binary64 最短往返格式及 int64 十进制格式均不超过 32 个 ASCII 字节。
inline constexpr std::size_t kScalarTextCapacity = 32;
[[nodiscard]] std::string_view FormatScalar(Value value,
                                            std::span<char, kScalarTextCapacity> buffer);
// 输入为有限标量；JSON 的 double 保留 .0 或指数，避免解码后退化为 int。
[[nodiscard]] std::string_view FormatJsonScalar(Value value,
                                                std::span<char, kScalarTextCapacity> buffer);
// 输入为 string/int/double/bool/null；join 与 sort 共用 JS 数值文本，null 为 "null"。
[[nodiscard]] std::string_view FormatArrayValue(Value value,
                                                std::span<char, kScalarTextCapacity> buffer);

void WriteValue(std::ostream& output, Value value);

}  // namespace vela::runtime
