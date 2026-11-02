#pragma once

#include <cstdint>
#include <string_view>

namespace vela::frontend {

enum class IntegerRadix : std::uint8_t { Binary = 2, Octal = 8, Decimal = 10, Hexadecimal = 16 };

[[nodiscard]] constexpr IntegerRadix IntegerLiteralRadix(std::string_view text) noexcept {
  if (text.starts_with("0x") || text.starts_with("0X")) {
    return IntegerRadix::Hexadecimal;
  }
  if (text.starts_with("0b") || text.starts_with("0B")) {
    return IntegerRadix::Binary;
  }
  if (text.starts_with("0o") || text.starts_with("0O")) {
    return IntegerRadix::Octal;
  }
  return IntegerRadix::Decimal;
}

}  // namespace vela::frontend
