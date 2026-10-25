#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "gtest/gtest.h"

#include "base/builtin.h"
#include "runtime/builtins.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

TEST(NumericConversionTest, PreservesInt64BoundaryAndExplicitRounding) {
  Heap heap;
  const std::array<Value, 1> minimum = {Value::Double(-0x1p63)};
  const auto converted = ExecuteBuiltin(Builtin::NumericToInt, minimum, heap);
  ASSERT_TRUE(converted.has_value());
  EXPECT_EQ(converted->AsInteger(), std::numeric_limits<std::int64_t>::min());
  const std::array<Value, 1> integer = {Value::Integer(9007199254740993LL)};
  const auto rounded = ExecuteBuiltin(Builtin::NumericToDouble, integer, heap);
  ASSERT_TRUE(rounded.has_value());
  EXPECT_DOUBLE_EQ(rounded->AsDouble(), 9007199254740992.0);
  const std::array<Value, 1> zero = {Value::Double(-0.0)};
  const auto preserved = ExecuteBuiltin(Builtin::NumericToDouble, zero, heap);
  ASSERT_TRUE(preserved.has_value());
  EXPECT_TRUE(std::signbit(preserved->AsDouble()));
  EXPECT_EQ(heap.object_count(), 0U);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
}

}  // namespace
}  // namespace vela::runtime
