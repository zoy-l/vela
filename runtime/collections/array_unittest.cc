#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "gtest/gtest.h"

#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

TEST(ArrayAccessTest, PreservesReceiverIndexAndValueErrorPrecedence) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* record = isolate.CreateClassLayout(0, {});
  auto* object = heap.AllocateInstance(*record);
  ASSERT_NE(object, nullptr);
  EXPECT_EQ(Heap::GetElement(*object, 0).error(), ArrayAccessError::NotArray);
  EXPECT_EQ(heap.SetElement(*object, 0, Value::Null()).error(), ArrayAccessError::NotArray);
  EXPECT_EQ(Heap::RemoveArrayElement(*object, 0).error(), ArrayAccessError::NotArray);
  const auto* layout = isolate.CreateArrayLayout({.element = {.kind = ValueKind::Integer}});
  auto* array = heap.AllocateArray(*layout, 1);
  ASSERT_NE(array, nullptr);
  ASSERT_TRUE(heap.SetElement(*array, 0, Value::Integer(42)));
  EXPECT_EQ(Heap::GetElement(*array, 1).error(), ArrayAccessError::OutOfBounds);
  EXPECT_EQ(heap.SetElement(*array, 1, Value::Null()).error(), ArrayAccessError::OutOfBounds);
  EXPECT_EQ(Heap::RemoveArrayElement(*array, 1).error(), ArrayAccessError::OutOfBounds);
  EXPECT_EQ(heap.FillArray(*array, Value::Null(), 1, 2).error(),
            ArrayAccessError::ElementTypeMismatch);
  EXPECT_EQ(Heap::ArrayLength(*array), 1U);
  EXPECT_EQ(Heap::GetElement(*array, 0)->AsInteger(), 42);
}

TEST(NullableArrayTest, StoresTaggedPrimitiveValuesWithoutConvertingThem) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* integers =
      isolate.CreateArrayLayout({.element = {.kind = ValueKind::Integer, .nullable = true}});
  constexpr std::size_t kLength = 1024;
  auto* array = heap.AllocateArray(*integers, kLength);
  ASSERT_NE(array, nullptr);
  const auto header_bytes = heap.allocated_bytes() - (kLength * sizeof(Value));
  EXPECT_GE(header_bytes, sizeof(HeapObject));
  EXPECT_LT(header_bytes, kLength);
  EXPECT_TRUE(Heap::GetElement(*array, 0)->IsNull());
  ASSERT_TRUE(heap.SetElement(*array, 0, Value::Integer(42)));
  EXPECT_EQ(Heap::GetElement(*array, 0)->AsInteger(), 42);
  EXPECT_EQ(heap.SetElement(*array, 0, Value::Double(42.0)).error(),
            ArrayAccessError::ElementTypeMismatch);
  ASSERT_TRUE(heap.SetElement(*array, 0, Value::Null()));
  EXPECT_TRUE(Heap::GetElement(*array, 0)->IsNull());
  EXPECT_EQ(heap.SetElement(*array, kLength, Value::Null()).error(), ArrayAccessError::OutOfBounds);
  const auto* strict = isolate.CreateArrayLayout({.element = {.kind = ValueKind::Integer}});
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
  const auto* records = isolate.CreateArrayLayout({
      .element =
          {
              .kind = ValueKind::Object,
              .nominal = &record->nominal_type(),
              .nullable = true,
          },
  });
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
      isolate.CreateArrayLayout({.element = {.kind = ValueKind::String, .nullable = true}});
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

TEST(DynamicArrayTest, GrowsGeometricallyAndPreservesAliasedIdentity) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  const auto* layout = isolate.CreateArrayLayout({.element = {.kind = ValueKind::Integer}});
  Value root = Value::Object(heap.AllocateArray(*layout, 0));
  RootRange roots(heap, &root, 1);
  auto* array = root.AsObject();
  constexpr std::uint32_t kCount = 4096;
  std::uint32_t growths = 0;
  auto capacity = *Heap::ArrayCapacity(*array);
  for (std::uint32_t index = 0; index < kCount; ++index) {
    ASSERT_EQ(heap.PushArray(*array, Value::Integer(index)), index + 1);
    const auto next_capacity = *Heap::ArrayCapacity(*array);
    if (next_capacity != capacity) {
      ++growths;
      capacity = next_capacity;
    }
  }
  EXPECT_EQ(array, root.AsObject());
  EXPECT_LT(growths, 16U);
  EXPECT_EQ(Heap::ArrayLength(*array), kCount);
  EXPECT_EQ(heap.object_count(), 1U);
}

TEST(DynamicArrayTest, NativeViewsObserveRelocatedCompactStorage) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* layout = isolate.CreateArrayLayout({.element = {.kind = ValueKind::Integer}});
  Value root = Value::Object(heap.AllocateArray(*layout, 0));
  RootRange roots(heap, &root, 1);
  auto* array = root.AsObject();
  ASSERT_TRUE(heap.PushArray(*array, Value::Integer(42)));
  ASSERT_TRUE(heap.ReserveArray(*array, 4096));
  const auto elements = Heap::IntegerElements(*array);
  ASSERT_TRUE(elements);
  EXPECT_EQ(elements->size(), 1U);
  EXPECT_EQ(elements->front(), 42);
}

TEST(DynamicArrayTest, ReserveAccountsStorageAndPreservesContentsAndMutationVersion) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* layout = isolate.CreateArrayLayout({.element = {.kind = ValueKind::Integer}});
  constexpr std::size_t kCount = 4096;
  Value root = Value::Object(heap.AllocateArray(*layout, kCount));
  RootRange roots(heap, &root, 1);
  auto* array = root.AsObject();
  const auto before = heap.allocated_bytes();
  const auto mutation = Heap::ArrayMutation(*array);
  ASSERT_TRUE(heap.ReserveArray(*array, kCount * 2));
  EXPECT_EQ(Heap::ArrayMutation(*array), mutation);
  EXPECT_EQ(heap.allocated_bytes() - before, kCount * sizeof(std::int64_t));
  EXPECT_EQ(Heap::GetElement(*array, kCount - 1)->AsInteger(), 0);
  EXPECT_EQ(Heap::GetElement(*array, kCount).error(), ArrayAccessError::OutOfBounds);
  EXPECT_EQ(heap.ReserveArray(
                    *array, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1)
                .error(),
            ArrayAccessError::OutOfMemory);
  EXPECT_EQ(Heap::ArrayLength(*array), kCount);
}

TEST(DynamicArrayTest, ShiftsTaggedValuesAndRejectsInvalidMutationsAtomically) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* layout =
      isolate.CreateArrayLayout({.element = {.kind = ValueKind::Integer, .nullable = true}});
  Value root = Value::Object(heap.AllocateArray(*layout, 0));
  RootRange roots(heap, &root, 1);
  auto& array = *root.AsObject();
  ASSERT_TRUE(heap.PushArray(array, Value::Integer(40)));
  ASSERT_TRUE(heap.PushArray(array, Value::Null()));
  Value input = Value::Object(heap.AllocateArray(*layout, 1));
  RootRange input_roots(heap, &input, 1);
  ASSERT_TRUE(heap.SetElement(*input.AsObject(), 0, Value::Integer(42)));
  ASSERT_TRUE(heap.ReplaceArrayRange(array, 1, 0, *input.AsObject()));
  EXPECT_EQ(Heap::RemoveArrayElement(array, 0)->AsInteger(), 40);
  EXPECT_EQ(Heap::GetElement(array, 0)->AsInteger(), 42);
  EXPECT_TRUE(Heap::PopArray(array)->IsNull());
  EXPECT_EQ(Heap::PopArray(array)->AsInteger(), 42);
  EXPECT_TRUE(Heap::PopArray(array)->IsNull());
  const auto mutation = Heap::ArrayMutation(array);
  EXPECT_EQ(heap.ReplaceArrayRange(array, 1, 0, *input.AsObject()).error(),
            ArrayAccessError::OutOfBounds);
  EXPECT_EQ(heap.PushArray(array, Value::Double(1.0)).error(),
            ArrayAccessError::ElementTypeMismatch);
  EXPECT_EQ(Heap::RemoveArrayElement(array, 0).error(), ArrayAccessError::OutOfBounds);
  EXPECT_EQ(Heap::ArrayMutation(array), mutation);
  EXPECT_EQ(Heap::ArrayLength(array), 0U);
}

class DynamicStringArrayTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto* layout = isolate_.CreateArrayLayout({.element = {.kind = ValueKind::String}});
    value_ = Value::Object(heap().AllocateArray(*layout, 0));
    ASSERT_NE(value_.AsObject(), nullptr);
  }
  Heap& heap() { return isolate_.heap(); }
  HeapObject& array() { return *value_.AsObject(); }
  void DropArray() { value_ = Value::Null(); }

 private:
  Isolate isolate_{1};
  Value value_;
  RootRange roots_{isolate_.heap(), &value_, 1};
};

TEST_F(DynamicStringArrayTest, RootsPendingReferencesDuringGrowthAndDropsRemovedElements) {
  auto& heap = this->heap();
  auto& array = this->array();
  constexpr std::uint32_t kCount = 512;
  for (std::uint32_t index = 0; index < kCount; ++index) {
    auto* string = heap.AllocateString("pending string");
    ASSERT_TRUE(heap.PushArray(array, Value::String(string)));
  }
  heap.FinishCollection();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), kCount + 1);
  ASSERT_TRUE(Heap::PopArray(array));
  ASSERT_TRUE(Heap::RemoveArrayElement(array, 0));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), kCount - 1);
}

TEST_F(DynamicStringArrayTest,
       ClearRetainsCapacityWithoutRetainingReferencesAndReleasesOwnedBuffers) {
  auto& heap = this->heap();
  auto& array = this->array();
  ASSERT_TRUE(heap.PushArray(array, Value::String(heap.AllocateString("removed"))));
  constexpr std::size_t kCount = 512;
  ASSERT_TRUE(heap.ReserveArray(array, kCount));
  const auto capacity = Heap::ArrayCapacity(array);
  ASSERT_TRUE(Heap::ClearArray(array));
  EXPECT_EQ(Heap::ArrayCapacity(array), capacity);
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
  const auto retained = heap.allocated_bytes();
  EXPECT_GT(retained, kCount * sizeof(StringObject*));
  DropArray();
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), 0U);
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(DynamicArrayTest, ReplacesReferenceRangesDuringSweep) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* record = isolate.CreateClassLayout(0, {});
  const auto* layout = isolate.CreateArrayLayout(
      {.element = {.kind = ValueKind::Object, .nominal = &record->nominal_type()}});
  auto* child = heap.AllocateInstance(*record);
  Value root = Value::Object(heap.AllocateArray(*layout, 0));
  RootRange roots(heap, &root, 1);
  Value input = Value::Object(heap.AllocateArray(*layout, 1));
  RootRange input_roots(heap, &input, 1);
  ASSERT_TRUE(heap.SetElement(*input.AsObject(), 0, Value::Object(child)));
  heap.StartCollection();
  // 标记两个数组及其共享元素，再推进一步进入 Sweep。
  heap.Step(heap.object_count() + 1);
  ASSERT_EQ(heap.phase(), GcPhase::Sweep);
  ASSERT_TRUE(heap.ReplaceArrayRange(*root.AsObject(), 0, 0, *input.AsObject()));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 3U);
  EXPECT_EQ(Heap::GetElement(*root.AsObject(), 0)->AsObject(), child);
}

class ArraySliceBarrierTest : public testing::TestWithParam<GcPhase> {};

TEST_P(ArraySliceBarrierTest, PublishesInitializedCopyDuringIncrementalCollection) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* record = isolate.CreateClassLayout(0, {});
  const auto* layout = isolate.CreateArrayLayout(
      {.element = {.kind = ValueKind::Object, .nominal = &record->nominal_type()}});
  constexpr std::size_t kBallast = 64;
  std::array<Value, kBallast> ballast;
  RootRange ballast_roots(heap, ballast.data(), ballast.size());
  for (auto& value : ballast) {
    value = Value::Object(heap.AllocateInstance(*record));
  }
  auto* child = heap.AllocateInstance(*record);
  std::array<Value, 2> values;
  RootRange roots(heap, values.data(), values.size());
  values.front() = Value::Object(heap.AllocateArray(*layout, 1));
  ASSERT_TRUE(heap.SetElement(*values.front().AsObject(), 0, Value::Object(child)));
  heap.StartCollection();
  if (GetParam() == GcPhase::Sweep) {
    heap.Step(heap.object_count() + 1);
  }
  ASSERT_EQ(heap.phase(), GetParam());
  const auto copy = heap.SliceArray(*values.front().AsObject(), 0, 1);
  ASSERT_TRUE(copy);
  values.back() = Value::Object(*copy);
  values.front() = Value::Null();
  ballast.fill(Value::Null());
  heap.FinishCollection();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(Heap::GetElement(**copy, 0)->AsObject(), child);
}

INSTANTIATE_TEST_SUITE_P(MarkingAndSweeping,
                         ArraySliceBarrierTest,
                         testing::Values(GcPhase::Mark, GcPhase::Sweep));

}  // namespace
}  // namespace vela::runtime
