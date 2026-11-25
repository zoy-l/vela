"""Private record arrays retain logical bounds, GC ownership and independent instances."""

SOURCE = """
class Point { x: double; count: int; }
let points: Point[] = [];
func prepare(): void {
  points.clear();
  points.reserve(8);
  for (let index = 0; index < 8; index++) {
    points.push(Point(x: index.toDouble() + 0.5, count: index + 1));
  }
}
func read(index: int): double {
  const point = points[index];
  return point.x + point.count.toDouble();
}
func update(rounds: int): void {
  for (let round = 0; round < rounds; round++) {
    for (const point in points) { point.x += 0.5; point.count += 1; }
  }
}
func setCount(value: int): void { points[0].count = value; }
func readX(): double { return points[0].x; }
export func calculate(mode: int, value: int): double {
  if (mode == 0) { prepare(); return 8.0; }
  if (mode == 1) { return read(value); }
  if (mode == 2) { update(value); return read(0); }
  if (mode == 3) { setCount(value); return 0.0; }
  if (mode == 4) { return readX(); }
  return -1.0;
}
"""

HOST_TESTS = r'''
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <gtest/gtest.h>
#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_control.h"
#include "runtime/value/value.h"

extern "C" const vela::runtime::NativeImage* RECORD_IMAGE() noexcept;

using namespace vela::runtime;

TEST(RecordArrayAotTest, PreservesBoundsAndRecordFieldsAcrossCollectionAndInstances) {
  Isolate isolate(1);
  const auto* image = RECORD_IMAGE();
  EXPECT_TRUE(std::ranges::any_of(image->arrays.values(), [](const auto& array) {
    return array.record_fields == 2;
  }));
  const auto first = NativeInstance::Instantiate(isolate, *image);
  const auto second = NativeInstance::Instantiate(isolate, *image);
  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(second) << second.error().message;
  const auto entry = (*first)->FindFunction("calculate");
  ASSERT_TRUE(entry);
  const auto run = [&](NativeInstance& instance, std::int64_t mode, std::int64_t value) {
    const std::array arguments{Value::Integer(mode), Value::Integer(value)};
    return ExecuteNativeFunction(instance, *entry, arguments);
  };
  ASSERT_TRUE(run(**first, 0, 0).Succeeded());
  isolate.heap().Collect();
  const auto initial = run(**first, 1, 0);
  ASSERT_TRUE(initial.Succeeded()) << initial.message;
  EXPECT_EQ(initial.value.AsDouble(), 1.5);
  const auto updated = run(**first, 2, 4);
  ASSERT_TRUE(updated.Succeeded()) << updated.message;
  EXPECT_EQ(updated.value.AsDouble(), 7.5);
  isolate.heap().Collect();
  EXPECT_EQ(run(**first, 1, 7).value.AsDouble(), 21.5);
  const std::array<std::int64_t, 3> indices{-1, 8, std::numeric_limits<std::int64_t>::max()};
  for (const auto index : indices) {
    const auto invalid = run(**first, 1, index);
    EXPECT_EQ(invalid.status, vela::ExecutionStatus::IndexOutOfBounds);
    EXPECT_FALSE(invalid.stack_trace.empty());
  }
  EXPECT_EQ(run(**second, 1, 0).status, vela::ExecutionStatus::IndexOutOfBounds);
  ASSERT_TRUE(run(**second, 0, 0).Succeeded());
  EXPECT_EQ(run(**second, 1, 0).value.AsDouble(), 1.5);
  EXPECT_EQ(run(**first, 1, 0).value.AsDouble(), 7.5);
  ASSERT_TRUE(run(**first, 0, 0).Succeeded());
  ASSERT_TRUE(run(**first, 3, std::numeric_limits<std::int64_t>::max() - 1).Succeeded());
  const auto overflow = run(**first, 2, 2);
  EXPECT_EQ(overflow.status, vela::ExecutionStatus::IntegerOverflow);
  EXPECT_FALSE(overflow.stack_trace.empty());
  EXPECT_EQ(run(**first, 4, 0).value.AsDouble(), 1.5);
  ASSERT_TRUE(run(**first, 0, 0).Succeeded());
  isolate.native_control().RequestStop();
  EXPECT_EQ(run(**first, 2, 10000).status, vela::ExecutionStatus::ExecutionStopped);
}
'''
