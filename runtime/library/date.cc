#include "runtime/library/date.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include <absl/strings/ascii.h>
#include <absl/time/civil_time.h>
#include <absl/time/time.h>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

using Fields = std::array<std::int64_t, kDatePartsFieldCount>;
using TimeResult = std::expected<absl::Time, DateFailure>;

DateFailure Failure(ExecutionStatus status, std::string_view message) {
  return {.status = status, .message = message};
}

TimeResult CheckedTime(absl::Time time) {
  if (time < absl::FromUnixMillis(kMinimumDateTimestamp) ||
      time > absl::FromUnixMillis(kMaximumDateTimestamp)) {
    return std::unexpected(Failure(ExecutionStatus::DateRangeError,
                                   "timestamp must be within years 0001 through 9999 in UTC"));
  }
  return time;
}

TimeResult FromFields(const Fields& fields, ExecutionStatus invalid) {
  constexpr Fields kMinimum = {1, 1, 1, 0, 0, 0, 0};
  constexpr Fields kMaximum = {9999, 12, 31, 23, 59, 59, 999};
  constexpr auto kMessages = std::to_array<std::string_view>({
      "year must be within 1..9999",
      "month must be within 1..12",
      "day must be valid for its year and month",
      "hour must be within 0..23",
      "minute must be within 0..59",
      "second must be within 0..59",
      "millisecond must be within 0..999",
  });
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto value = fields.at(index);
    if (value < kMinimum.at(index) || value > kMaximum.at(index)) {
      return std::unexpected(Failure(invalid, kMessages.at(index)));
    }
  }
  const auto& [year, month, day, hour, minute, second, millisecond] = fields;
  const absl::CivilSecond civil(year, static_cast<int>(month), static_cast<int>(day),
                                static_cast<int>(hour), static_cast<int>(minute),
                                static_cast<int>(second));
  // CivilSecond 会规范化无效日期；在接受结果前核对原日历字段。
  if (civil.year() != year || civil.month() != month || civil.day() != day) {
    return std::unexpected(Failure(invalid, kMessages.at(2)));
  }
  return absl::FromCivil(civil, absl::UTCTimeZone()) + absl::Milliseconds(millisecond);
}

bool IsDigits(std::string_view text) {
  return std::ranges::all_of(text, absl::ascii_isdigit);
}

constexpr std::int64_t kDecimalRadix = 10;
constexpr std::int64_t kMinutesPerHour = 60;
constexpr std::int64_t kMaximumOffsetHour = 23;
constexpr std::size_t kMaximumFractionDigits = 3;
constexpr std::string_view kCalendarShape = "0000-00-00T00:00:00";
constexpr std::string_view kCalendarSeparators = "--T::";
constexpr std::string_view kOffsetShape = "+00:00";

std::int64_t ReadDigits(std::string_view text) {
  std::int64_t result = 0;
  for (const auto byte : text) {
    result = (result * kDecimalRadix) + (byte - '0');
  }
  return result;
}

std::expected<Fields, DateFailure> ParseFields(std::string_view& remaining) {
  const auto invalid = ExecutionStatus::DateParseError;
  Fields fields{};
  for (std::size_t index = 0; index < fields.size() - 1; ++index) {
    const auto width = index == 0 ? 4U : 2U;
    const auto number = remaining.substr(0, width);
    if (!IsDigits(number)) {
      return std::unexpected(Failure(invalid, "date and time fields must contain ASCII digits"));
    }
    fields.at(index) = ReadDigits(number);
    remaining.remove_prefix(width);
    if (index < kCalendarSeparators.size()) {
      if (remaining.front() != kCalendarSeparators.substr(index).front()) {
        return std::unexpected(Failure(invalid, "expected YYYY-MM-DDTHH:mm:ss"));
      }
      remaining.remove_prefix(1);
    }
  }
  if (remaining.starts_with('.')) {
    remaining.remove_prefix(1);
    const auto digits = remaining.find_first_not_of("0123456789");
    if (digits == std::string_view::npos || digits == 0 || digits > kMaximumFractionDigits) {
      return std::unexpected(
          Failure(invalid, "fractional seconds require one through three digits"));
    }
    fields.back() = ReadDigits(remaining.substr(0, digits));
    for (auto count = digits; count < kMaximumFractionDigits; ++count) {
      fields.back() *= kDecimalRadix;
    }
    remaining.remove_prefix(digits);
  }
  return fields;
}

std::expected<std::int64_t, DateFailure> ParseOffset(std::string_view suffix) {
  if (suffix == "Z") {
    return 0;
  }
  const auto invalid = Failure(ExecutionStatus::DateParseError, "offset must be Z or signed HH:mm");
  if (suffix.size() != kOffsetShape.size() || (suffix.front() != '+' && suffix.front() != '-') ||
      suffix.substr(3).front() != ':') {
    return std::unexpected(invalid);
  }
  const auto hour_text = suffix.substr(1, 2);
  const auto minute_text = suffix.substr(kOffsetShape.size() - 2);
  if (!IsDigits(hour_text) || !IsDigits(minute_text)) {
    return std::unexpected(invalid);
  }
  const auto hours = ReadDigits(hour_text);
  const auto minutes = ReadDigits(minute_text);
  const auto negative = suffix.front() == '-';
  if (hours > kMaximumOffsetHour || minutes >= kMinutesPerHour ||
      (negative && hours == 0 && minutes == 0)) {
    return std::unexpected(Failure(ExecutionStatus::DateParseError,
                                   "invalid numeric offset; -00:00 is unknown, not UTC"));
  }
  return ((hours * kMinutesPerHour) + minutes) * (negative ? -1 : 1);
}

TimeResult Parse(std::string_view text) {
  constexpr auto kMinimumLength = kCalendarShape.size() + 1;
  constexpr auto kMaximumLength =
      kCalendarShape.size() + 1 + kMaximumFractionDigits + kOffsetShape.size();
  if (text.size() < kMinimumLength || text.size() > kMaximumLength) {
    return std::unexpected(Failure(ExecutionStatus::DateParseError,
                                   "expected YYYY-MM-DDTHH:mm:ss with an explicit offset"));
  }
  const auto fields = ParseFields(text);
  if (!fields) {
    return std::unexpected(fields.error());
  }
  const auto offset = ParseOffset(text);
  if (!offset) {
    return std::unexpected(offset.error());
  }
  const auto local = FromFields(*fields, ExecutionStatus::DateParseError);
  if (!local) {
    return local;
  }
  return CheckedTime(*local - absl::Minutes(*offset));
}

TimeResult Timestamp(Value value) {
  if (!value.IsInteger()) {
    return std::unexpected(Failure(ExecutionStatus::TypeError, "timestamp must be int"));
  }
  return CheckedTime(absl::FromUnixMillis(value.AsInteger()));
}

Fields Decompose(absl::Time time) {
  const auto civil = absl::ToCivilSecond(time, absl::UTCTimeZone());
  const auto start = absl::FromCivil(civil, absl::UTCTimeZone());
  return {
      civil.year(),
      civil.month(),
      civil.day(),
      civil.hour(),
      civil.minute(),
      civil.second(),
      absl::ToInt64Milliseconds(time - start),
  };
}

}  // namespace

std::expected<Value, DateFailure> ExecuteDateBuiltin(Builtin builtin,
                                                     std::span<const Value> arguments,
                                                     Heap& heap,
                                                     const ClassLayout* date_parts_layout) {
  if (builtin == Builtin::DateParse) {
    const auto source = arguments.front();
    if (!source.IsString()) {
      return std::unexpected(Failure(ExecutionStatus::TypeError, "date source must be string"));
    }
    const auto result = Parse(source.AsString()->value());
    if (!result) {
      return std::unexpected(result.error());
    }
    return Value::Integer(absl::ToUnixMillis(*result));
  }
  if (builtin == Builtin::DateFromUTC) {
    Fields fields{};
    for (std::size_t index = 0; index < fields.size(); ++index) {
      const auto value = arguments.subspan(index).front();
      if (!value.IsInteger()) {
        return std::unexpected(Failure(ExecutionStatus::TypeError, "UTC components must be int"));
      }
      fields.at(index) = value.AsInteger();
    }
    const auto result = FromFields(fields, ExecutionStatus::DateRangeError);
    if (!result) {
      return std::unexpected(result.error());
    }
    return Value::Integer(absl::ToUnixMillis(*result));
  }
  const auto time = Timestamp(arguments.front());
  if (!time) {
    return std::unexpected(time.error());
  }
  if (builtin == Builtin::DateToISOString) {
    const auto text = absl::FormatTime("%E4Y-%m-%dT%H:%M:%E3SZ", *time, absl::UTCTimeZone());
    auto* output = heap.AllocateString(text);
    if (output == nullptr) {
      return std::unexpected(Failure(ExecutionStatus::OutOfMemory, ""));
    }
    return Value::String(output);
  }
  const auto fields = Decompose(*time);
  std::array<Value, kDatePartsFieldCount> values;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    values.at(index) = Value::Integer(fields.at(index));
  }
  auto* output = heap.AllocateInstance(*date_parts_layout, values);
  if (output == nullptr) {
    return std::unexpected(Failure(ExecutionStatus::OutOfMemory, ""));
  }
  return Value::Object(output);
}

}  // namespace vela::runtime
