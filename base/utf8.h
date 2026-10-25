#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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

// 输入边界验证返回首个非法字节的位置；内部字符串操作依赖合法 UTF-8 契约。
[[nodiscard]] std::optional<std::size_t> InvalidUtf8Offset(std::string_view text) noexcept;
void AppendUtf8(std::uint32_t point, std::string& text);
[[nodiscard]] std::size_t Utf8Length(std::string_view text) noexcept;
[[nodiscard]] std::optional<std::size_t> Utf8Offset(std::string_view text,
                                                    std::size_t index) noexcept;

}  // namespace vela
