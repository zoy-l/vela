#include <cmath>
#include <cstdint>
#include <limits>

#include "gtest/gtest.h"

#include "runtime/library/builtins.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

TEST(NumericConversionTest, PreservesInt64BoundaryAndExplicitRounding) {
  const auto converted = ConvertNumber(NumericConversion::ToInt, Value::Double(-0x1p63));
  ASSERT_TRUE(converted.has_value());
  EXPECT_EQ(converted->AsInteger(), std::numeric_limits<std::int64_t>::min());
  const auto rounded =
      ConvertNumber(NumericConversion::ToDouble, Value::Integer(9007199254740993LL));
  ASSERT_TRUE(rounded.has_value());
  EXPECT_DOUBLE_EQ(rounded->AsDouble(), 9007199254740992.0);
  const auto preserved = ConvertNumber(NumericConversion::ToDouble, Value::Double(-0.0));
  ASSERT_TRUE(preserved.has_value());
  EXPECT_TRUE(std::signbit(preserved->AsDouble()));
}

}  // namespace
}  // namespace vela::runtime
