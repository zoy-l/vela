#include <cstddef>

#include "gtest/gtest.h"

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

TEST(NullableArrayTest, StoresTaggedPrimitiveValuesWithoutConvertingThem) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* integers =
      isolate.CreateArrayLayout({.element_kind = ValueKind::Integer, .nullable_elements = true});
  constexpr std::size_t kLength = 1024;
  auto* array = heap.AllocateArray(*integers, kLength);
  ASSERT_NE(array, nullptr);
  EXPECT_EQ(heap.allocated_bytes(), sizeof(HeapObject) + (kLength * sizeof(Value)));
  EXPECT_TRUE(Heap::GetElement(*array, 0)->IsNull());
  ASSERT_TRUE(heap.SetElement(*array, 0, Value::Integer(42)));
  EXPECT_EQ(Heap::GetElement(*array, 0)->AsInteger(), 42);
  EXPECT_EQ(heap.SetElement(*array, 0, Value::Double(42.0)).error(),
            ArrayAccessError::ElementTypeMismatch);
  ASSERT_TRUE(heap.SetElement(*array, 0, Value::Null()));
  EXPECT_TRUE(Heap::GetElement(*array, 0)->IsNull());
  EXPECT_EQ(heap.SetElement(*array, kLength, Value::Null()).error(), ArrayAccessError::OutOfBounds);
  const auto* strict = isolate.CreateArrayLayout({.element_kind = ValueKind::Integer});
  auto* strict_array = heap.AllocateArray(*strict, 1);
  ASSERT_NE(strict_array, nullptr);
  EXPECT_EQ(heap.SetElement(*strict_array, 0, Value::Null()).error(),
            ArrayAccessError::ElementTypeMismatch);
}

TEST(NullableArrayTest, KeepsReferenceElementsCompactAndChecksTheirLayouts) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* record = isolate.CreateClassLayout(0, {});
  auto* other = isolate.CreateClassLayout(0, {});
  const auto* records = isolate.CreateArrayLayout(
      {.element_kind = ValueKind::Object, .element_class = record, .nullable_elements = true});
  EXPECT_EQ(records->element_size(), sizeof(HeapObject*));
  Value root = Value::Object(heap.AllocateArray(*records, 1));
  RootRange roots(heap, &root, 1);
  ASSERT_TRUE(heap.SetElement(*root.AsObject(), 0, Value::Null()));
  auto* wrong = heap.AllocateInstance(*other);
  EXPECT_EQ(heap.SetElement(*root.AsObject(), 0, Value::Object(wrong)).error(),
            ArrayAccessError::ElementTypeMismatch);
  auto* child = heap.AllocateInstance(*record);
  heap.StartCollection();
  heap.Step(1);
  ASSERT_TRUE(heap.SetElement(*root.AsObject(), 0, Value::Object(child)));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(Heap::GetElement(*root.AsObject(), 0)->AsObject(), child);
  ASSERT_TRUE(heap.SetElement(*root.AsObject(), 0, Value::Null()));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
}

TEST(NullableArrayTest, RetainsStringElementsDuringIncrementalSweep) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* strings =
      isolate.CreateArrayLayout({.element_kind = ValueKind::String, .nullable_elements = true});
  EXPECT_EQ(strings->element_size(), sizeof(StringObject*));
  auto* string = heap.AllocateString("retained");
  Value root = Value::Object(heap.AllocateArray(*strings, 1));
  RootRange roots(heap, &root, 1);
  heap.StartCollection();
  heap.Step(2);
  ASSERT_EQ(heap.phase(), GcPhase::Sweep);
  ASSERT_TRUE(heap.SetElement(*root.AsObject(), 0, Value::String(string)));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(Heap::GetElement(*root.AsObject(), 0)->AsString(), string);
  ASSERT_TRUE(heap.SetElement(*root.AsObject(), 0, Value::Null()));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
}

}  // namespace
}  // namespace vela::runtime
