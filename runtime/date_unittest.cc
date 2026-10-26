#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "gtest/gtest.h"

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/date.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

struct DateTextSample final {
  std::string_view text;
  std::string_view canonical;
};

class DateTextTest : public testing::TestWithParam<DateTextSample> {};

TEST_P(DateTextTest, ParsesExactPrecisionAndExplicitOffsets) {
  const auto& sample = GetParam();
  Isolate isolate(1);
  auto& heap = isolate.heap();
  const std::array<Value, 1> source{Value::String(heap.AllocateString(sample.text))};
  const auto parsed = ExecuteDateBuiltin(Builtin::DateParse, source, heap, nullptr);
  ASSERT_TRUE(parsed);
  const std::array<Value, 1> timestamp{*parsed};
  const auto formatted = ExecuteDateBuiltin(Builtin::DateToISOString, timestamp, heap, nullptr);
  ASSERT_TRUE(formatted);
  EXPECT_EQ(formatted->AsString()->value(), sample.canonical);
  const std::array<Value, 1> encoded{*formatted};
  const auto round_trip = ExecuteDateBuiltin(Builtin::DateParse, encoded, heap, nullptr);
  ASSERT_TRUE(round_trip);
  EXPECT_EQ(round_trip->AsInteger(), parsed->AsInteger());
}

INSTANTIATE_TEST_SUITE_P(CanonicalUTC,
                         DateTextTest,
                         testing::ValuesIn(std::to_array<DateTextSample>({
                             {"1970-01-01T00:00:00Z", "1970-01-01T00:00:00.000Z"},
                             {"1969-12-31T23:59:59.999Z", "1969-12-31T23:59:59.999Z"},
                             {"0001-01-01T00:00:00Z", "0001-01-01T00:00:00.000Z"},
                             {"9999-12-31T23:59:59.999Z", "9999-12-31T23:59:59.999Z"},
                             {"0099-01-01T00:00:00Z", "0099-01-01T00:00:00.000Z"},
                             {"2026-10-26T12:30:45.1+08:00", "2026-10-26T04:30:45.100Z"},
                             {"2026-10-26T12:30:45.12+00:00", "2026-10-26T12:30:45.120Z"},
                             {"2026-01-01T00:30:00-23:59", "2026-01-02T00:29:00.000Z"},
                             {"2026-01-01T00:30:00+23:59", "2025-12-31T00:31:00.000Z"},
                             {"2000-02-29T00:00:00Z", "2000-02-29T00:00:00.000Z"},
                         })));

TEST(DateTest, RejectsMalformedOrNormalizedCalendarInputs) {
  constexpr auto inputs = std::to_array<std::string_view>({
      "",
      "2026-01-01",
      "2026-01-01T00:00:00",
      "2026-01-01t00:00:00Z",
      "2026-01-01T00:00:00z",
      "2026-01-01T00:00:00-00:00",
      "2026-01-01T00:00:00+24:00",
      "2026-01-01T00:00:00+00:60",
      "2026-01-01T00:00:00.0000Z",
      "2026-01-01T00:00:00.Z",
      "2026-01-01T00:00:60Z",
      "2026-01-01T24:00:00Z",
      "0000-01-01T00:00:00Z",
      "1900-02-29T00:00:00Z",
      "2026-02-30T00:00:00Z",
      "2026-13-01T00:00:00Z",
      "2026-00-01T00:00:00Z",
      " 2026-01-01T00:00:00Z",
      "2026-01-01T00:00:00Z ",
      "2026-01-01T00:00:00Zjunk",
      std::string_view("2026-01-01T00:00:00Z\0", 21),
      "２０２６-01-01T00:00:00Z",
  });
  for (const auto text : inputs) {
    Heap heap;
    const std::array<Value, 1> arguments{Value::String(heap.AllocateString(text))};
    const auto result = ExecuteDateBuiltin(Builtin::DateParse, arguments, heap, nullptr);
    ASSERT_FALSE(result) << text;
    EXPECT_EQ(result.error().status, ExecutionStatus::DateParseError) << text;
  }
}

TEST(DateTest, RejectsNormalizedUTCInstantsOutsideTheSupportedYears) {
  Heap heap;
  for (const auto* text : {"0001-01-01T00:00:00+00:01", "9999-12-31T23:59:59.999-00:01"}) {
    const std::array<Value, 1> arguments{Value::String(heap.AllocateString(text))};
    const auto result = ExecuteDateBuiltin(Builtin::DateParse, arguments, heap, nullptr);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().status, ExecutionStatus::DateRangeError);
  }
}

TEST(DateTest, RejectsOutOfRangeTimestamps) {
  Heap heap;
  for (const auto timestamp : {
           kMinimumDateTimestamp - 1,
           kMaximumDateTimestamp + 1,
           std::numeric_limits<std::int64_t>::min(),
           std::numeric_limits<std::int64_t>::max(),
       }) {
    const std::array<Value, 1> arguments{Value::Integer(timestamp)};
    const auto result = ExecuteDateBuiltin(Builtin::DateToISOString, arguments, heap, nullptr);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().status, ExecutionStatus::DateRangeError);
  }
}

class DateTimestampTest : public testing::TestWithParam<std::int64_t> {};

TEST_P(DateTimestampTest, DecompositionRoundTripsAndUsesFloorBasedMilliseconds) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  const auto timestamp = GetParam();
  const std::array<Value, 2> input{Value::Integer(timestamp), Value::Null()};
  const auto parts =
      ExecuteDateBuiltin(Builtin::DateToUTC, input, heap, &isolate.date_parts_layout());
  ASSERT_TRUE(parts);
  RootRange root(heap, &*parts, 1);
  heap.Collect();
  std::array<Value, kDatePartsFieldCount> fields;
  for (std::uint32_t index = 0; index < fields.size(); ++index) {
    fields.at(index) = *Heap::GetField(*parts->AsObject(), index);
  }
  if (timestamp == -1) {
    EXPECT_EQ(fields.back().AsInteger(), 999);
  }
  const auto restored = ExecuteDateBuiltin(Builtin::DateFromUTC, fields, heap, nullptr);
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->AsInteger(), timestamp);
}

INSTANTIATE_TEST_SUITE_P(Instants,
                         DateTimestampTest,
                         testing::ValuesIn(std::to_array<std::int64_t>({kMinimumDateTimestamp, -1,
                                                                        0, 1, 951782400123LL,
                                                                        kMaximumDateTimestamp})));

class DateFieldRangeTest : public testing::TestWithParam<std::size_t> {};

TEST_P(DateFieldRangeTest, ValidatesEachUTCComponentBeforeNarrowing) {
  Heap heap;
  const std::array<Value, kDatePartsFieldCount> valid{
      Value::Integer(2000), Value::Integer(2),  Value::Integer(29),  Value::Integer(23),
      Value::Integer(59),   Value::Integer(59), Value::Integer(999),
  };
  for (const auto invalid :
       {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
    auto fields = valid;
    fields.at(GetParam()) = Value::Integer(invalid);
    const auto result = ExecuteDateBuiltin(Builtin::DateFromUTC, fields, heap, nullptr);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().status, ExecutionStatus::DateRangeError);
    EXPECT_FALSE(result.error().message.empty());
  }
}

INSTANTIATE_TEST_SUITE_P(Components,
                         DateFieldRangeTest,
                         testing::Range(std::size_t{0}, std::size_t{kDatePartsFieldCount}));

}  // namespace
}  // namespace vela::runtime
