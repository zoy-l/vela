#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

TEST(HeapAllocationTest, BudgetRejectsManagedAndExternalAllocationsWithoutChangingAccounting) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* type = isolate.CreateClassLayout(1, {});
  const auto* array = isolate.CreateArrayLayout({.element = {.kind = ValueKind::Integer}});
  heap.SetAllocationLimit(0);
  EXPECT_EQ(heap.AllocateInstance(*type), nullptr);
  EXPECT_EQ(heap.AllocateString("value"), nullptr);
  EXPECT_EQ(heap.AllocateNative(1), nullptr);
  EXPECT_EQ(heap.AllocateValueSlots(1), nullptr);
  EXPECT_EQ(heap.AllocateArray(*array, 1), nullptr);
  EXPECT_EQ(heap.AllocateArray(*array, 16), nullptr);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
  EXPECT_EQ(heap.object_count(), 0U);
  constexpr std::size_t kExternalElements = 16;
  heap.SetAllocationLimit(kExternalElements * array->element_size());
  EXPECT_EQ(heap.AllocateArray(*array, kExternalElements), nullptr);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
  EXPECT_EQ(heap.object_count(), 0U);
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  ASSERT_NE(heap.AllocateInstance(*type), nullptr);
}

TEST(HeapAllocationTest, FailedGrowthPreservesArraySlotsStringAndCollectionState) {
  Isolate isolate;
  auto& heap = isolate.heap();
  std::array<Value, 4> values;
  const RootRange roots(heap, values.data(), values.size());
  const auto* array = isolate.CreateArrayLayout({.element = {.kind = ValueKind::Integer}});
  const auto* map =
      isolate.CreateCollectionLayout({.keys = {.element = {.kind = ValueKind::Integer}}});
  values.at(0) = Value::Object(heap.AllocateArray(*array, 1));
  values.at(1) = Value::Object(heap.AllocateValueSlots(1));
  values.at(2) = Value::Object(heap.AllocateMutableString());
  values.at(3) = Value::Object(heap.AllocateCollection(*map));
  auto& elements = *values.at(0).AsObject();
  auto& slots = *values.at(1).AsObject();
  auto& text = *values.at(2).AsObject();
  auto& entries = *values.at(3).AsObject();
  constexpr std::int64_t kRetainedValue = 42;
  ASSERT_TRUE(heap.SetElement(elements, 0, Value::Integer(kRetainedValue)));
  heap.SetValueSlot(slots, 0, Value::Integer(kRetainedValue));
  const auto bytes = heap.allocated_bytes();
  heap.SetAllocationLimit(bytes);
  const auto reserved = heap.ReserveArray(elements, 32);
  ASSERT_FALSE(reserved);
  EXPECT_EQ(reserved.error(), ArrayAccessError::OutOfMemory);
  const auto extended = heap.ExtendValueSlots(slots, 32);
  ASSERT_FALSE(extended);
  EXPECT_EQ(extended.error(), ExecutionStatus::OutOfMemory);
  const auto appended = heap.AppendString(text, "value");
  ASSERT_FALSE(appended);
  EXPECT_EQ(appended.error(), ExecutionStatus::OutOfMemory);
  const auto inserted = heap.InsertCollection(entries, Value::Integer(1));
  ASSERT_FALSE(inserted);
  EXPECT_EQ(inserted.error(), ExecutionStatus::OutOfMemory);
  EXPECT_EQ(heap.allocated_bytes(), bytes);
  EXPECT_EQ(Heap::GetElement(elements, 0)->AsInteger(), 42);
  EXPECT_EQ(Heap::ValueSlots(slots).front().AsInteger(), 42);
  EXPECT_EQ(Heap::ValueSlots(slots).size(), 1U);
  EXPECT_EQ(Heap::CollectionLength(entries), 0U);
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  ASSERT_TRUE(heap.ReserveArray(elements, 32));
  ASSERT_TRUE(heap.ExtendValueSlots(slots, 32));
  ASSERT_TRUE(heap.AppendString(text, "value"));
  ASSERT_TRUE(heap.InsertCollection(entries, Value::Integer(1)));
  EXPECT_EQ(Heap::GetElement(elements, 0)->AsInteger(), 42);
  EXPECT_EQ(Heap::ValueSlots(slots).front().AsInteger(), 42);
}

}  // namespace
}  // namespace vela::runtime
