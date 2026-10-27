#include "runtime/numeric_parse.h"

#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>

#include <absl/strings/ascii.h>
#include <absl/strings/numbers.h>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

constexpr std::int64_t kMinimumRadix = 2;
constexpr std::int64_t kMaximumRadix = 36;
constexpr std::int64_t kDecimalRadix = 10;
constexpr std::int64_t kHexadecimalRadix = 16;

int Digit(char byte) {
  if (byte >= '0' && byte <= '9') {
    return byte - '0';
  }
  if (byte >= 'a' && byte <= 'z') {
    return byte - 'a' + static_cast<int>(kDecimalRadix);
  }
  return byte >= 'A' && byte <= 'Z' ? byte - 'A' + static_cast<int>(kDecimalRadix) : -1;
}

std::expected<std::int64_t, ExecutionStatus> ParseInt(std::string_view source, Value radix_value) {
  if (!radix_value.IsNull() && !radix_value.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto explicit_radix = !radix_value.IsNull();
  auto radix = explicit_radix ? radix_value.AsInteger() : kDecimalRadix;
  if (radix < kMinimumRadix || radix > kMaximumRadix) {
    return std::unexpected(ExecutionStatus::InvalidRadix);
  }
  auto text = absl::StripAsciiWhitespace(source);
  bool negative = false;
  if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
    negative = text.front() == '-';
    text.remove_prefix(1);
  }
  if (!explicit_radix && (text.starts_with("0x") || text.starts_with("0X"))) {
    radix = kHexadecimalRadix;
    text.remove_prefix(2);
  }
  if (text.empty()) {
    return std::unexpected(ExecutionStatus::IntParseError);
  }
  const auto limit =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + (negative ? 1U : 0U);
  const auto base = static_cast<std::uint64_t>(radix);
  std::uint64_t magnitude = 0;
  for (const auto byte : text) {
    const auto digit = Digit(byte);
    if (digit < 0 || digit >= radix) {
      return std::unexpected(ExecutionStatus::IntParseError);
    }
    const auto value = static_cast<std::uint64_t>(digit);
    if (magnitude > (limit - value) / base) {
      return std::unexpected(ExecutionStatus::IntParseError);
    }
    magnitude = (magnitude * base) + value;
  }
  if (negative && magnitude == limit) {
    return std::numeric_limits<std::int64_t>::min();
  }
  const auto result = static_cast<std::int64_t>(magnitude);
  return negative ? -result : result;
}

bool IsDecimal(std::string_view text) {
  if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
    text.remove_prefix(1);
  }
  const auto digits = [&text]() {
    const auto size = text.size();
    while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
      text.remove_prefix(1);
    }
    return size - text.size();
  };
  auto count = digits();
  if (!text.empty() && text.front() == '.') {
    text.remove_prefix(1);
    count += digits();
  }
  if (count == 0) {
    return false;
  }
  if (!text.empty() && (text.front() == 'e' || text.front() == 'E')) {
    text.remove_prefix(1);
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
      text.remove_prefix(1);
    }
    if (digits() == 0) {
      return false;
    }
  }
  return text.empty();
}

std::expected<double, ExecutionStatus> ParseDouble(std::string_view source) {
  const auto text = absl::StripAsciiWhitespace(source);
  auto payload = text;
  bool negative = false;
  if (!payload.empty() && (payload.front() == '+' || payload.front() == '-')) {
    negative = payload.front() == '-';
    payload.remove_prefix(1);
  }
  if (payload == "NaN") {
    return std::copysign(std::numeric_limits<double>::quiet_NaN(), negative ? -1.0 : 1.0);
  }
  if (payload == "Infinity") {
    return negative ? -std::numeric_limits<double>::infinity()
                    : std::numeric_limits<double>::infinity();
  }
  double result = 0.0;
  if (!IsDecimal(text) || !absl::SimpleAtod(text, &result)) {
    return std::unexpected(ExecutionStatus::DoubleParseError);
  }
  return result;
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteNumericParse(Builtin builtin,
                                                          std::span<const Value> arguments) {
  const auto source = arguments.front();
  if (!source.IsString()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto text = source.AsString()->value();
  if (builtin == Builtin::IntParse || builtin == Builtin::IntTryParse) {
    const auto result = ParseInt(text, arguments.subspan(1).front());
    if (result) {
      return Value::Integer(*result);
    }
    if (builtin == Builtin::IntTryParse && result.error() == ExecutionStatus::IntParseError) {
      return Value::Null();
    }
    return std::unexpected(result.error());
  }
  const auto result = ParseDouble(text);
  if (result) {
    return Value::Double(*result);
  }
  if (builtin == Builtin::DoubleTryParse) {
    return Value::Null();
  }
  return std::unexpected(result.error());
}

}  // namespace vela::runtime
