#include "runtime/value_io.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <memory>
#include <ostream>
#include <span>
#include <string_view>
#include <utility>

#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

constexpr int kMinimumFixedExponent = -6;
constexpr int kMaximumFixedExponent = 21;

std::span<char> WriteArrayNumberDigits(std::span<const char> digits,
                                       int exponent,
                                       std::span<char> output) {
  const auto append = [&](char character) -> void {
    output.front() = character;
    output = output.subspan(1);
  };
  if (exponent >= kMinimumFixedExponent && exponent < kMaximumFixedExponent) {
    if (exponent < 0) {
      append('0');
      append('.');
      for (int index = -1; index > exponent; --index) {
        append('0');
      }
    }
    for (std::size_t index = 0; index < digits.size(); ++index) {
      if (exponent >= 0 && std::cmp_equal(index, exponent + 1)) {
        append('.');
      }
      append(digits.subspan(index).front());
    }
    for (auto index = static_cast<int>(digits.size()); index <= exponent; ++index) {
      append('0');
    }
    return output;
  }
  append(digits.front());
  if (digits.size() > 1) {
    append('.');
    for (const auto digit : digits.subspan(1)) {
      append(digit);
    }
  }
  append('e');
  append(exponent < 0 ? '-' : '+');
  const auto result =
      std::to_chars(output.data(), std::to_address(output.end()), std::abs(exponent));
  return output.subspan(static_cast<std::size_t>(result.ptr - output.data()));
}

}  // namespace

std::string_view FormatArrayValue(Value value, std::span<char, kScalarTextCapacity> buffer) {
  if (value.IsString()) {
    return value.AsString()->value();
  }
  if (value.IsNull()) {
    return "null";
  }
  if (!value.IsDouble()) {
    return FormatScalar(value, buffer);
  }
  const auto number = value.AsDouble();
  if (std::isnan(number)) {
    return "NaN";
  }
  if (std::isinf(number)) {
    return number < 0 ? "-Infinity" : "Infinity";
  }
  if (number == 0) {
    return "0";
  }
  const auto converted = std::to_chars(buffer.data(), std::to_address(buffer.end()), number,
                                       std::chars_format::general);
  const std::string_view text(buffer.data(),
                              static_cast<std::size_t>(converted.ptr - buffer.data()));
  const auto marker = text.find('e');
  if (marker == std::string_view::npos) {
    return text;
  }
  auto exponent_text = text.substr(marker + 1);
  if (exponent_text.front() == '+') {
    exponent_text.remove_prefix(1);
  }
  int exponent = 0;
  static_cast<void>(std::from_chars(std::to_address(exponent_text.begin()),
                                    std::to_address(exponent_text.end()), exponent));
  std::array<char, kScalarTextCapacity> digits{};
  std::size_t count = 0;
  const auto sign = number < 0 ? 1U : 0U;
  for (const auto digit : text.substr(sign, marker - sign)) {
    if (digit != '.') {
      digits.at(count++) = digit;
    }
  }
  std::span<char> remaining = buffer;
  if (number < 0) {
    remaining.front() = '-';
    remaining = remaining.subspan(1);
  }
  remaining = WriteArrayNumberDigits(std::span(digits).first(count), exponent, remaining);
  return {buffer.data(), buffer.size() - remaining.size()};
}

std::string_view FormatScalar(Value value, std::span<char, kScalarTextCapacity> buffer) {
  if (value.IsBoolean()) {
    return value.AsBoolean() ? "true" : "false";
  }
  const auto converted =
      value.IsInteger()
          ? std::to_chars(buffer.data(), std::to_address(buffer.end()), value.AsInteger())
          : std::to_chars(buffer.data(), std::to_address(buffer.end()), value.AsDouble());
  return {buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())};
}

void WriteValue(std::ostream& output, Value value) {
  if (value.IsInteger() || value.IsDouble() || value.IsBoolean()) {
    std::array<char, kScalarTextCapacity> buffer{};
    output << FormatScalar(value, buffer);
  } else if (value.IsString()) {
    output << value.AsString()->value();
  } else if (value.IsNull()) {
    output << "null";
  } else {
    output << "object";
  }
}

std::string_view FormatJsonScalar(Value value, std::span<char, kScalarTextCapacity> buffer) {
  const auto text = FormatScalar(value, buffer);
  if (!value.IsDouble() || text.find_first_of(".eE") != std::string_view::npos) {
    return text;
  }
  auto remaining = buffer.subspan(text.size());
  remaining.front() = '.';
  remaining.subspan(1).front() = '0';
  return {buffer.data(), text.size() + 2};
}

}  // namespace vela::runtime
