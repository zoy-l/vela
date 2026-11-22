#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "runtime/heap/block_page_memory.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_memory.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

TEST(HeapAllocationTest, BlockPagesHaveIndependentAlignedWritableStorage) {
  auto* first = internal::AllocateHeapBlockPage();
  auto* second = internal::AllocateHeapBlockPage();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first, second);
  constexpr auto kPageBytes = internal::kHeapBlockPageBytes;
  EXPECT_EQ(std::bit_cast<std::uintptr_t>(first) % kPageBytes, 0U);
  EXPECT_EQ(std::bit_cast<std::uintptr_t>(second) % kPageBytes, 0U);
  const std::span first_bytes(static_cast<std::byte*>(first), kPageBytes);
  const std::span second_bytes(static_cast<std::byte*>(second), kPageBytes);
  constexpr auto kFirstValue = std::byte{0x42};
  constexpr auto kSecondValue = std::byte{0x17};
  first_bytes.front() = kFirstValue;
  first_bytes.back() = kFirstValue;
  second_bytes.front() = kSecondValue;
  second_bytes.back() = kSecondValue;
  internal::ReleaseHeapBlockPage(first);
  EXPECT_EQ(second_bytes.front(), kSecondValue);
  EXPECT_EQ(second_bytes.back(), kSecondValue);
  internal::ReleaseHeapBlockPage(second);
}

TEST(HeapAllocationTest, HostDrivenCollectionDoesNotAdvanceDuringAllocation) {
  constexpr std::size_t kDiscardedObjects = 64U;
  Isolate isolate(1U, GcScheduling::kHostDriven);
  auto& heap = isolate.heap();
  const auto* layout = isolate.CreateClassLayout(0U, {});
  for (std::size_t index = 0U; index < kDiscardedObjects; ++index) {
    ASSERT_NE(heap.AllocateInstance(*layout), nullptr);
  }
  EXPECT_EQ(heap.phase(), GcPhase::Mark);
  EXPECT_EQ(heap.object_count(), kDiscardedObjects);
  // 本周期发布的新对象在下一周期才成为垃圾；两个周期都由宿主明确推进。
  heap.FinishCollection();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(HeapAllocationTest, HostDrivenStepPreservesObjectsPublishedDuringSweep) {
  constexpr std::size_t kRetainedObjects = 64U;
  constexpr std::int64_t kPayload = 42;
  Isolate isolate(1U, GcScheduling::kHostDriven);
  auto& heap = isolate.heap();
  const auto* layout = isolate.CreateClassLayout(1U, {});
  std::array<Value, kRetainedObjects> values;
  const RootRange roots(heap, values.data(), values.size());
  for (auto& value : values) {
    auto* object = heap.AllocateInstance(*layout);
    ASSERT_NE(object, nullptr);
    value = Value::Object(object);
  }
  // 分配达到阈值只开始 Mark；只有宿主提供的工作预算才能推进这个周期。
  EXPECT_EQ(heap.phase(), GcPhase::Mark);
  EXPECT_EQ(heap.object_count(), kRetainedObjects);
  heap.Step(1U);
  EXPECT_EQ(heap.phase(), GcPhase::Mark);
  heap.FinishCollection();
  EXPECT_EQ(heap.phase(), GcPhase::Idle);
  values.fill(Value::Null());
  heap.StartCollection();
  while (heap.phase() == GcPhase::Mark) {
    heap.Step(1U);
  }
  ASSERT_EQ(heap.phase(), GcPhase::Sweep);
  auto* published = heap.AllocateInstance(*layout);
  ASSERT_NE(published, nullptr);
  values.front() = Value::Object(published);
  ASSERT_TRUE(heap.SetField(*published, 0U, Value::Integer(kPayload)));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 1U);
  const auto field = Heap::GetField(*published, 0U);
  ASSERT_TRUE(field);
  EXPECT_EQ(field->AsInteger(), kPayload);
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
  values.fill(Value::Null());
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(HeapAllocationTest, ReclaimsEmptyPagesIncrementallyAfterWorkingSetShrinks) {
  constexpr std::size_t kObjects = 4096U;
  constexpr std::size_t kMinimumCache = 1024U;
  Isolate isolate(kMinimumCache, GcScheduling::kHostDriven);
  auto& heap = isolate.heap();
  const auto* layout = isolate.CreateClassLayout(1U, {});
  constexpr std::uint32_t kLargerFieldCount = 32U;
  const auto* larger = isolate.CreateClassLayout(kLargerFieldCount, {});
  std::array<Value, kObjects> values;
  const RootRange roots(heap, values.data(), values.size());
  values.front() = Value::Object(heap.AllocateInstance(*layout));
  const auto single_page = heap.committed_bytes();
  for (std::size_t index = 1U; index < values.size(); ++index) {
    values[index] = Value::Object(heap.AllocateInstance(index % 2U == 0U ? *layout : *larger));
  }
  heap.FinishCollection();
  const auto high_water = heap.committed_bytes();
  const auto first = values.front();
  const auto last = values.back();
  constexpr std::int64_t kFirstPayload = 17;
  constexpr std::int64_t kLastPayload = 29;
  ASSERT_TRUE(heap.SetField(*first.AsObject(), 0U, Value::Integer(kFirstPayload)));
  ASSERT_TRUE(heap.SetField(*last.AsObject(), 0U, Value::Integer(kLastPayload)));
  values.fill(Value::Null());
  values.front() = first;
  values.back() = last;
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_LT(heap.committed_bytes(), high_water);
  EXPECT_EQ(Heap::LoadField(*first.AsObject(), 0U).AsInteger(), kFirstPayload);
  EXPECT_EQ(Heap::LoadField(*last.AsObject(), 0U).AsInteger(), kLastPayload);
  values.fill(Value::Null());
  heap.StartCollection();
  while (heap.object_count() != 0U) {
    heap.Step(1U);
  }
  const auto before_release = heap.committed_bytes();
  heap.Step(1U);
  EXPECT_LT(heap.committed_bytes(), before_release);
  EXPECT_TRUE(heap.IsCollecting());
  // 空页释放期间的新对象仍遵循 Sweep 的发布和根屏障契约。
  auto* retained = heap.AllocateInstance(*layout);
  ASSERT_NE(retained, nullptr);
  values.front() = Value::Object(retained);
  heap.FinishCollection();
  EXPECT_LT(heap.committed_bytes(), high_water);
  EXPECT_LE(heap.committed_bytes(), single_page + kMinimumCache);
  EXPECT_EQ(heap.object_count(), 1U);
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
}

TEST(HeapAllocationTest, ExternalResourcePressureDoesNotRetainUnusedObjectPages) {
  constexpr std::size_t kMinimumCache = 1024U;
  constexpr std::size_t kExternalBytes = std::size_t{4} * 1024U * 1024U;
  constexpr std::size_t kObjects = 4096U;
  Isolate isolate(kMinimumCache, GcScheduling::kHostDriven);
  auto& heap = isolate.heap();
  const HostMemory external(heap, kExternalBytes);
  ASSERT_TRUE(external.IsReserved());
  const auto* layout = isolate.CreateClassLayout(1U, {});
  std::array<Value, kObjects> values;
  const RootRange roots(heap, values.data(), values.size());
  for (auto& value : values) {
    auto* object = heap.AllocateInstance(*layout);
    ASSERT_NE(object, nullptr);
    value = Value::Object(object);
  }
  heap.FinishCollection();
  EXPECT_GT(heap.committed_bytes(), kExternalBytes);
  values.fill(Value::Null());
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
  EXPECT_EQ(heap.allocated_bytes(), kExternalBytes);
  EXPECT_EQ(heap.committed_bytes(), kExternalBytes);
}

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
  ASSERT_TRUE(heap.AppendString(text, "value"));
  ASSERT_TRUE(heap.InsertCollection(entries, Value::Integer(1)));
  EXPECT_EQ(Heap::GetElement(elements, 0)->AsInteger(), 42);
  EXPECT_EQ(Heap::ValueSlots(slots).front().AsInteger(), 42);
}

}  // namespace
}  // namespace vela::runtime
