#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <sstream>

#include "gtest/gtest.h"

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"
#include "runtime/value_io.h"

namespace vela::runtime {
namespace {

TEST(RuntimeTest, CollectsUnreachableInstances) {
  Isolate isolate;
  constexpr std::array<std::uint32_t, 1> references = {1};
  auto* layout = isolate.CreateClassLayout(2, references);
  Heap& heap = isolate.heap();

  HeapObject* child = heap.AllocateInstance(*layout);
  Value root_value = Value::Object(heap.AllocateInstance(*layout));
  ASSERT_TRUE(heap.SetField(*root_value.AsObject(), 1, Value::Object(child)));
  RootRange roots(heap, &root_value, 1);

  EXPECT_EQ(heap.object_count(), 2U);

  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);

  root_value = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(RuntimeTest, CollectsCyclesWithoutRoots) {
  Isolate isolate;
  constexpr std::array<std::uint32_t, 1> references = {0};
  auto* layout = isolate.CreateClassLayout(1, references);
  Heap& heap = isolate.heap();

  Value first = Value::Object(heap.AllocateInstance(*layout));
  Value second = Value::Object(heap.AllocateInstance(*layout));
  RootRange roots(heap, &first, 1);
  ASSERT_TRUE(heap.SetField(*first.AsObject(), 0, second));
  ASSERT_TRUE(heap.SetField(*second.AsObject(), 0, first));

  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);

  first = Value::Null();
  second = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(RuntimeTest, WriteBarrierKeepsNewReferenceAlive) {
  Isolate isolate;
  constexpr std::array<std::uint32_t, 1> references = {0};
  auto* layout = isolate.CreateClassLayout(1, references);
  Heap& heap = isolate.heap();

  Value parent = Value::Object(heap.AllocateInstance(*layout));
  RootRange roots(heap, &parent, 1);
  HeapObject* child = heap.AllocateInstance(*layout);

  heap.StartCollection();
  heap.Step(1);
  ASSERT_EQ(heap.phase(), GcPhase::Mark);

  ASSERT_TRUE(heap.SetField(*parent.AsObject(), 0, Value::Object(child)));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 2U);

  parent = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(RuntimeTest, FreeListReusesStorage) {
  Isolate isolate;
  constexpr std::array<std::uint32_t, 0> references = {};
  auto* layout = isolate.CreateClassLayout(1, references);
  Heap& heap = isolate.heap();

  HeapObject* first = heap.AllocateInstance(*layout);
  const std::size_t committed_before_collection = heap.committed_bytes();
  Value root = Value::Object(first);
  RootRange roots(heap, &root, 1);
  root = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);

  HeapObject* second = heap.AllocateInstance(*layout);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(heap.committed_bytes(), committed_before_collection);
}

TEST(RuntimeTest, ArrayStorageSurvivesCollectionAndReuse) {
  Isolate isolate;
  Heap& heap = isolate.heap();
  auto* layout = isolate.CreateClassLayout(0, std::span<const std::uint32_t>());
  const auto* array_layout =
      isolate.CreateArrayLayout({.element = {.kind = ValueKind::Object, .instance = layout}});
  Value array = Value::Object(heap.AllocateArray(*array_layout, 1));
  RootRange roots(heap, &array, 1);
  auto* child = heap.AllocateInstance(*layout);
  ASSERT_TRUE(heap.SetElement(*array.AsObject(), 0, Value::Object(child)));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  const auto element = Heap::GetElement(*array.AsObject(), 0);
  ASSERT_TRUE(element.has_value());
  ASSERT_TRUE(element->IsObject());
  EXPECT_EQ(element->AsObject(), child);
  array = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
  array = Value::Object(heap.AllocateArray(*array_layout, 2));
  const auto first = Heap::GetElement(*array.AsObject(), 0);
  const auto second = Heap::GetElement(*array.AsObject(), 1);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_TRUE(first->IsNull());
  EXPECT_TRUE(second->IsNull());
}

TEST(RuntimeTest, TypedArraysUseCompactAccountedStorage) {
  Isolate isolate;
  auto& heap = isolate.heap();
  constexpr std::size_t kLength = 1024;
  const auto* booleans = isolate.CreateArrayLayout({.element = {.kind = ValueKind::Boolean}});
  const auto* integers = isolate.CreateArrayLayout({.element = {.kind = ValueKind::Integer}});
  const auto* doubles = isolate.CreateArrayLayout({.element = {.kind = ValueKind::Double}});
  auto* array = heap.AllocateArray(*booleans, kLength);
  ASSERT_NE(array, nullptr);
  const auto boolean_bytes = heap.allocated_bytes();
  const auto header_bytes = boolean_bytes - (kLength * sizeof(bool));
  EXPECT_GE(boolean_bytes, sizeof(HeapObject) + kLength);
  EXPECT_LT(boolean_bytes, kLength * sizeof(Value));
  ASSERT_TRUE(heap.SetElement(*array, 0, Value::Boolean(true)));
  EXPECT_TRUE(Heap::GetElement(*array, 0)->AsBoolean());
  EXPECT_EQ(heap.SetElement(*array, 0, Value::Integer(1)).error(),
            ArrayAccessError::ElementTypeMismatch);
  EXPECT_EQ(Heap::GetElement(*array, kLength).error(), ArrayAccessError::OutOfBounds);
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), 0U);
  array = heap.AllocateArray(*integers, kLength);
  ASSERT_NE(array, nullptr);
  EXPECT_EQ(heap.allocated_bytes(), header_bytes + (kLength * sizeof(std::int64_t)));
  EXPECT_EQ(Heap::GetElement(*array, 0)->AsInteger(), 0);
  heap.Collect();
  array = heap.AllocateArray(*doubles, 1);
  ASSERT_NE(array, nullptr);
  ASSERT_TRUE(heap.SetElement(*array, 0, Value::Double(1.5)));
  EXPECT_DOUBLE_EQ(Heap::GetElement(*array, 0)->AsDouble(), 1.5);
  EXPECT_EQ(heap.SetElement(*array, 0, Value::Integer(1)).error(),
            ArrayAccessError::ElementTypeMismatch);
  heap.Collect();
  array = heap.AllocateArray(*doubles, kLength);
  ASSERT_NE(array, nullptr);
  EXPECT_EQ(heap.allocated_bytes(), header_bytes + (kLength * sizeof(double)));
}

TEST(RuntimeTest, NestedArrayWriteBarrierRetainsChildrenAndRejectsWrongLayouts) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* record = isolate.CreateClassLayout(0, {});
  auto* other_record = isolate.CreateClassLayout(0, {});
  const auto* records =
      isolate.CreateArrayLayout({.element = {.kind = ValueKind::Object, .instance = record}});
  const auto* nested =
      isolate.CreateArrayLayout({.element = {.kind = ValueKind::Object, .array = records}});
  Value root = Value::Object(heap.AllocateArray(*nested, 1));
  RootRange roots(heap, &root, 1);
  auto* inner = heap.AllocateArray(*records, 1);
  auto* child = heap.AllocateInstance(*record);
  auto* wrong_child = heap.AllocateInstance(*other_record);
  EXPECT_EQ(heap.SetElement(*inner, 0, Value::Object(wrong_child)).error(),
            ArrayAccessError::ElementTypeMismatch);
  EXPECT_EQ(heap.SetElement(*root.AsObject(), 0, Value::Object(child)).error(),
            ArrayAccessError::ElementTypeMismatch);
  heap.StartCollection();
  heap.Step(1);
  ASSERT_TRUE(heap.SetElement(*root.AsObject(), 0, Value::Object(inner)));
  ASSERT_TRUE(heap.SetElement(*inner, 0, Value::Object(child)));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 3U);
  root = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(RuntimeTest, ArrayWriteBarrierRetainsUnsweptChild) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* record = isolate.CreateClassLayout(0, {});
  const auto* records =
      isolate.CreateArrayLayout({.element = {.kind = ValueKind::Object, .instance = record}});
  auto* child = heap.AllocateInstance(*record);
  Value root = Value::Object(heap.AllocateArray(*records, 1));
  RootRange roots(heap, &root, 1);
  heap.StartCollection();
  heap.Step(2);
  ASSERT_EQ(heap.phase(), GcPhase::Sweep);
  ASSERT_TRUE(heap.SetElement(*root.AsObject(), 0, Value::Object(child)));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(Heap::GetElement(*root.AsObject(), 0)->AsObject(), child);
}

TEST(RuntimeTest, WritesDoubleWithRoundTripPrecisionAndSignedZero) {
  constexpr double kRoundTripValue = 1.2345678901234567;
  constexpr double kShortestDecimal = 0.1;
  std::ostringstream output;
  WriteValue(output, Value::Double(kRoundTripValue));
  output << ' ';
  WriteValue(output, Value::Double(-0.0));
  output << ' ';
  WriteValue(output, Value::Double(kShortestDecimal));
  EXPECT_EQ(output.str(), "1.2345678901234567 -0 0.1");
}

}  // namespace
}  // namespace vela::runtime
