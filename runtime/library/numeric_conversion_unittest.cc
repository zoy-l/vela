#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "gtest/gtest.h"

#include "base/builtin.h"
#include "runtime/isolate/isolate.h"
#include "runtime/library/builtins.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

TEST(NumericConversionTest, PreservesInt64BoundaryAndExplicitRounding) {
  Isolate isolate;
  const auto convert = [&](Builtin builtin, Value value) {
    return ExecuteBuiltin(builtin, std::array{value}, isolate.heap());
  };
  const auto converted = convert(Builtin::NumericToInt, Value::Double(-0x1p63));
  ASSERT_TRUE(converted.has_value());
  EXPECT_EQ(converted->AsInteger(), std::numeric_limits<std::int64_t>::min());
  const auto rounded = convert(Builtin::NumericToDouble, Value::Integer(9007199254740993LL));
  ASSERT_TRUE(rounded.has_value());
  EXPECT_DOUBLE_EQ(rounded->AsDouble(), 9007199254740992.0);
  const auto preserved = convert(Builtin::NumericToDouble, Value::Double(-0.0));
  ASSERT_TRUE(preserved.has_value());
  EXPECT_TRUE(std::signbit(preserved->AsDouble()));
}

}  // namespace
}  // namespace vela::runtime
