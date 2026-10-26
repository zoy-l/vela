#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

#include "gtest/gtest.h"

#include "base/builtin.h"
#include "runtime/builtins.h"
#include "runtime/heap.h"
#include "runtime/numeric_parse.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

TEST(NumericParseTest, Int64LimitsSignsAndRadices) {
  struct Sample {
    std::string_view text;
    Value radix;
    std::int64_t expected;
  };
  const auto samples = std::to_array<Sample>({
      {
          .text = "9223372036854775807",
          .radix = Value::Null(),
          .expected = std::numeric_limits<std::int64_t>::max(),
      },
      {
          .text = "-9223372036854775808",
          .radix = Value::Null(),
          .expected = std::numeric_limits<std::int64_t>::min(),
      },
      {
          .text = "-0X8000000000000000",
          .radix = Value::Null(),
          .expected = std::numeric_limits<std::int64_t>::min(),
      },
      {.text = " \t+42\r\n", .radix = Value::Null(), .expected = 42},
      {.text = "-0", .radix = Value::Null(), .expected = 0},
      {.text = "0xff", .radix = Value::Null(), .expected = 255},
      {.text = "101010", .radix = Value::Integer(2), .expected = 42},
      {.text = "-2A", .radix = Value::Integer(16), .expected = -42},
      {.text = "z", .radix = Value::Integer(36), .expected = 35},
  });
  for (const auto& sample : samples) {
    Heap heap;
    const std::array<Value, 2> arguments{
        Value::String(heap.AllocateString(sample.text)),
        sample.radix,
    };
    const auto before = heap.allocated_bytes();
    const auto result = ExecuteNumericParse(Builtin::IntParse, arguments);
    ASSERT_TRUE(result) << sample.text;
    EXPECT_EQ(result->AsInteger(), sample.expected);
    EXPECT_EQ(heap.allocated_bytes(), before);
  }
}

TEST(NumericParseTest, InvalidAndOverflowingIntegersAreNotTruncated) {
  constexpr auto inputs = std::to_array<std::string_view>({
      "",
      " ",
      "+",
      "-",
      "0x",
      "1_000",
      "1.0",
      "42suffix",
      "--1",
      "+-1",
      "0b10",
      "9223372036854775808",
      "-9223372036854775809",
      "0xffffffffffffffff",
      "１２",
      std::string_view("42\0", 3),
      "\xc2\xa0"
      "42",
  });
  for (const auto text : inputs) {
    Heap heap;
    const std::array<Value, 2> arguments{Value::String(heap.AllocateString(text)), Value::Null()};
    const auto result = ExecuteNumericParse(Builtin::IntParse, arguments);
    ASSERT_FALSE(result) << text;
    EXPECT_EQ(result.error(), BuiltinError::IntParseError);
    const auto tried = ExecuteNumericParse(Builtin::IntTryParse, arguments);
    ASSERT_TRUE(tried);
    EXPECT_TRUE(tried->IsNull());
  }
}

TEST(NumericParseTest, InvalidRadixIsAnErrorEvenForTryParse) {
  Heap heap;
  for (const auto radix : std::to_array<std::int64_t>({
           std::numeric_limits<std::int64_t>::min(),
           -1,
           0,
           1,
           37,
           std::numeric_limits<std::int64_t>::max(),
       })) {
    const std::array<Value, 2> arguments{
        Value::String(heap.AllocateString("")),
        Value::Integer(radix),
    };
    const auto tried = ExecuteNumericParse(Builtin::IntTryParse, arguments);
    ASSERT_FALSE(tried);
    EXPECT_EQ(tried.error(), BuiltinError::InvalidRadix);
  }
}

TEST(NumericParseTest, Binary64PrecisionSubnormalsSignedZeroAndNonFiniteValues) {
  struct Sample {
    std::string_view text;
    double expected;
  };
  const auto samples = std::to_array<Sample>({
      {.text = ".5", .expected = 0.5},
      {.text = "1.", .expected = 1.0},
      {.text = " \t+1.25e2\n", .expected = 125.0},
      {.text = "9007199254740993", .expected = 9007199254740992.0},
      {.text = "1.7976931348623157e308", .expected = std::numeric_limits<double>::max()},
      {.text = "4.9406564584124654e-324", .expected = std::numeric_limits<double>::denorm_min()},
      {.text = "1e9999", .expected = std::numeric_limits<double>::infinity()},
      {.text = "-Infinity", .expected = -std::numeric_limits<double>::infinity()},
      {.text = "Infinity", .expected = std::numeric_limits<double>::infinity()},
      {.text = "-0.0", .expected = -0.0},
      {.text = "-1e-9999", .expected = -0.0},
  });
  for (const auto& sample : samples) {
    Heap heap;
    const std::array<Value, 1> arguments{Value::String(heap.AllocateString(sample.text))};
    const auto result = ExecuteNumericParse(Builtin::DoubleParse, arguments);
    ASSERT_TRUE(result) << sample.text;
    EXPECT_EQ(result->AsDouble(), sample.expected) << sample.text;
    EXPECT_EQ(std::signbit(result->AsDouble()), std::signbit(sample.expected));
  }
}

TEST(NumericParseTest, SignedNaNIsAValidTryParseResult) {
  Heap heap;
  for (const auto* const text : {"NaN", "+NaN", "-NaN"}) {
    const std::array<Value, 1> arguments{Value::String(heap.AllocateString(text))};
    const auto result = ExecuteNumericParse(Builtin::DoubleTryParse, arguments);
    ASSERT_TRUE(result);
    EXPECT_TRUE(std::isnan(result->AsDouble()));
  }
}

TEST(NumericParseTest, DoublesRequireTheCompleteDecimalGrammar) {
  constexpr auto inputs = std::to_array<std::string_view>({
      "",
      " ",
      ".",
      "1e",
      "1e+",
      "0x1p2",
      "1,5",
      "inf",
      "nan",
      "infinity",
      "NaN(x)",
      "1 2",
      "1_000",
      "--1",
      "+-1",
      "1.2suffix",
      std::string_view("1\0", 2),
      "１２",
  });
  for (const auto text : inputs) {
    Heap heap;
    const std::array<Value, 1> arguments{Value::String(heap.AllocateString(text))};
    const auto parsed = ExecuteNumericParse(Builtin::DoubleParse, arguments);
    ASSERT_FALSE(parsed) << text;
    EXPECT_EQ(parsed.error(), BuiltinError::DoubleParseError);
    const auto tried = ExecuteNumericParse(Builtin::DoubleTryParse, arguments);
    ASSERT_TRUE(tried);
    EXPECT_TRUE(tried->IsNull());
  }
}

}  // namespace
}  // namespace vela::runtime
