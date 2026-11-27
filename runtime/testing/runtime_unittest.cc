#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <utility>

#include "base/execution_status.h"
#include "base/inline_layout.h"
#include "gtest/gtest.h"
#include "runtime/async/future.h"

#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_io.h"

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
  const Value second = Value::Object(heap.AllocateInstance(*layout));
  RootRange roots(heap, &first, 1);
  ASSERT_TRUE(heap.SetField(*first.AsObject(), 0, second));
  ASSERT_TRUE(heap.SetField(*second.AsObject(), 0, first));

  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);

  first = Value::Null();
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
  auto* layout = isolate.CreateClassLayout(1, {});
  Heap& heap = isolate.heap();

  ASSERT_NE(heap.AllocateInstance(*layout), nullptr);
  const std::size_t committed_before_collection = heap.committed_bytes();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);

  ASSERT_NE(heap.AllocateInstance(*layout), nullptr);
  EXPECT_EQ(heap.committed_bytes(), committed_before_collection);
}

TEST(RuntimeTest, ArrayStorageSurvivesCollectionAndReuse) {
  Isolate isolate;
  Heap& heap = isolate.heap();
  auto* layout = isolate.CreateClassLayout(0, {});
  const auto* array_layout = isolate.CreateArrayLayout(
      {.element = {.kind = ValueKind::Object, .nominal = &layout->nominal_type()}});
  Value array = Value::Object(heap.AllocateArray(*array_layout, 1));
  RootRange roots(heap, &array, 1);
  auto* child = heap.AllocateInstance(*layout);
  ASSERT_TRUE(heap.SetElement(*array.AsObject(), 0, Value::Object(child)));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  const auto element = heap.GetElement(*array.AsObject(), 0);
  ASSERT_TRUE(element.has_value());
  ASSERT_TRUE(element->IsObject());
  EXPECT_EQ(element->AsObject(), child);
  array = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
  array = Value::Object(heap.AllocateArray(*array_layout, 2));
  const auto first = heap.GetElement(*array.AsObject(), 0);
  const auto second = heap.GetElement(*array.AsObject(), 1);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_TRUE(first->IsNull());
  EXPECT_TRUE(second->IsNull());
}

TEST(RuntimeTest, FlatStructArraysKeepScalarRecordsInlineAndBoxIndependentSnapshots) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  ClassLayoutOptions options;
  options.nominal =
      isolate.CreateNominalType({.kind = NominalKind::Struct, .interfaces = {}, .declaration = {}});
  options.value_slots = {InlineSlotKind::Double, InlineSlotKind::Double, InlineSlotKind::Double};
  const auto* position = isolate.CreateClassLayout(3, {}, std::move(options));
  const auto* layout = isolate.CreateArrayLayout({
      .element = {.kind = ValueKind::Object, .nominal = &position->nominal_type()},
      .value_layout = position,
  });
  EXPECT_EQ(layout->element_size(), 24U);
  EXPECT_FALSE(layout->HasReferences());
  std::array values{Value::Object(heap.AllocateArray(*layout, 0)), Value::Null()};
  RootRange roots(heap, values.data(), values.size());
  const std::array fields{Value::Double(10), Value::Double(20), Value::Double(30)};
  values[1] = Value::Object(heap.AllocateInstance(*position, fields));
  auto& array = *values[0].AsObject();
  constexpr std::uint32_t kElements = 12;
  for (std::uint32_t index = 0; index < kElements; ++index) {
    ASSERT_TRUE(heap.PushArray(array, values[1]));
  }
  ASSERT_TRUE(heap.SetField(*values[1].AsObject(), 0, Value::Double(-1)));
  values[1] = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
  const auto copied = heap.GetElement(array, 5);
  ASSERT_TRUE(copied);
  values[1] = *copied;
  EXPECT_DOUBLE_EQ(Heap::LoadField(*copied->AsObject(), 0).AsDouble(), 10);
  ASSERT_TRUE(heap.SetField(*copied->AsObject(), 0, Value::Double(42)));
  const auto unchanged = heap.GetElement(array, 5);
  ASSERT_TRUE(unchanged);
  EXPECT_DOUBLE_EQ(Heap::LoadField(*unchanged->AsObject(), 0).AsDouble(), 10);
  heap.SetAllocationCountLimit(0);
  EXPECT_EQ(heap.GetElement(array, 0).error(), ArrayAccessError::OutOfMemory);
  EXPECT_EQ(heap.PopArray(array).error(), ArrayAccessError::OutOfMemory);
  EXPECT_EQ(*Heap::ArrayLength(array), 12U);
}

TEST(RuntimeTest, FlatStructArraysTraceReferenceLeavesAndKeepNullableRecordsIndependent) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  ClassLayoutOptions options;
  options.nominal =
      isolate.CreateNominalType({.kind = NominalKind::Struct, .interfaces = {}, .declaration = {}});
  options.value_slots = {
      InlineSlotKind::Boolean,
      InlineSlotKind::String,
      InlineSlotKind::NullableInteger,
      InlineSlotKind::Dynamic,
  };
  const std::array<std::uint32_t, 2> references{1, 3};
  const auto* record = isolate.CreateClassLayout(4, references, std::move(options));
  const auto* layout = isolate.CreateArrayLayout({
      .element = {.kind = ValueKind::Object, .nominal = &record->nominal_type(), .nullable = true},
      .value_layout = record,
  });
  EXPECT_EQ(record->value_layout().size(), 48U);
  EXPECT_EQ(layout->element_size(), 56U);
  EXPECT_TRUE(layout->HasReferences());
  std::array values{
      Value::Object(heap.AllocateArray(*layout, 2)),
      Value::Null(),
      Value::Null(),
      Value::Null(),
  };
  RootRange roots(heap, values.data(), values.size());
  values[1] = Value::String(heap.AllocateString("name"));
  values[2] = Value::String(heap.AllocateString("dynamic"));
  const std::array fields{Value::Boolean(true), values[1], Value::Integer(42), values[2]};
  values[3] = Value::Object(heap.AllocateInstance(*record, fields));
  auto& array = *values[0].AsObject();
  ASSERT_TRUE(heap.SetElement(array, 1, values[3]));
  values[1] = Value::Null();
  values[2] = Value::Null();
  values[3] = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 3U);
  ASSERT_TRUE(heap.GetElement(array, 0)->IsNull());
  const auto copied = heap.GetElement(array, 1);
  ASSERT_TRUE(copied);
  values[3] = *copied;
  EXPECT_EQ(Heap::LoadField(*copied->AsObject(), 1).AsString()->value(), "name");
  EXPECT_EQ(Heap::LoadField(*copied->AsObject(), 2).AsInteger(), 42);
  EXPECT_EQ(Heap::LoadField(*copied->AsObject(), 3).AsString()->value(), "dynamic");
  ASSERT_TRUE(heap.FillArray(array, *copied, 0, 2));
  ASSERT_TRUE(heap.ReverseArray(array));
  ASSERT_TRUE(heap.SetElement(array, 0, Value::Null()));
  EXPECT_TRUE(heap.GetElement(array, 0)->IsNull());
  heap.Collect();
  EXPECT_EQ(Heap::LoadField(*heap.GetElement(array, 1)->AsObject(), 2).AsInteger(), 42);
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
  EXPECT_TRUE(heap.GetElement(*array, 0)->AsBoolean());
  EXPECT_EQ(heap.SetElement(*array, 0, Value::Integer(1)).error(),
            ArrayAccessError::ElementTypeMismatch);
  EXPECT_EQ(heap.GetElement(*array, kLength).error(), ArrayAccessError::OutOfBounds);
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), 0U);
  array = heap.AllocateArray(*integers, kLength);
  ASSERT_NE(array, nullptr);
  EXPECT_EQ(heap.allocated_bytes(), header_bytes + (kLength * sizeof(std::int64_t)));
  EXPECT_EQ(heap.GetElement(*array, 0)->AsInteger(), 0);
  heap.Collect();
  array = heap.AllocateArray(*doubles, 1);
  ASSERT_NE(array, nullptr);
  ASSERT_TRUE(heap.SetElement(*array, 0, Value::Double(1.5)));
  EXPECT_DOUBLE_EQ(heap.GetElement(*array, 0)->AsDouble(), 1.5);
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
  const auto* records = isolate.CreateArrayLayout(
      {.element = {.kind = ValueKind::Object, .nominal = &record->nominal_type()}});
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
  const auto* records = isolate.CreateArrayLayout(
      {.element = {.kind = ValueKind::Object, .nominal = &record->nominal_type()}});
  auto* child = heap.AllocateInstance(*record);
  Value root = Value::Object(heap.AllocateArray(*records, 1));
  RootRange roots(heap, &root, 1);
  heap.StartCollection();
  heap.Step(2);
  ASSERT_EQ(heap.phase(), GcPhase::Sweep);
  ASSERT_TRUE(heap.SetElement(*root.AsObject(), 0, Value::Object(child)));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(heap.GetElement(*root.AsObject(), 0)->AsObject(), child);
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

TEST(RuntimeTest, FutureBindingStoresThePayloadSignatureAndRejectsRetyping) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto initial_array_layouts = isolate.layout_counts().arrays;
  const auto* integer = isolate.CreateTypeSignature(
      {.parameters = {}, .result = TypeConstraint{.kind = ValueKind::Integer}});
  const auto* text = isolate.CreateTypeSignature(
      {.parameters = {}, .result = TypeConstraint{.kind = ValueKind::String}});
  const TypeConstraint type{
      .kind = ValueKind::Object,
      .object_kind = ObjectKind::Future,
      .signature = integer,
  };
  const auto value = Value::Object(heap.AllocateFuture(Future(&isolate.scheduler())));
  RootRange root(heap, &value, 1);
  auto& object = *value.AsObject();
  EXPECT_EQ(Heap::TypeOf(value).signature, nullptr);
  ASSERT_TRUE(Heap::BindRuntimeType(object, type));
  EXPECT_EQ(Heap::TypeOf(value), type);
  EXPECT_EQ(isolate.layout_counts().arrays, initial_array_layouts);
  ASSERT_TRUE(Heap::BindRuntimeType(object, type));
  EXPECT_EQ(
      Heap::BindRuntimeType(
          object, {.kind = ValueKind::Object, .object_kind = ObjectKind::Future, .signature = text})
          .error(),
      ExecutionStatus::InvalidCast);
  EXPECT_EQ(Heap::BindRuntimeType(object, {.kind = ValueKind::Integer}).error(),
            ExecutionStatus::TypeError);
}

TEST(RuntimeTest, FlatteningNullableFutureOfNeverProducesNullType) {
  const TypeSignature payload{
      .parameters = {},
      .result = TypeConstraint{.category = TypeConstraint::Category::Never},
  };
  const TypeSignature outer{
      .parameters = {},
      .result =
          TypeConstraint{
              .kind = ValueKind::Object,
              .object_kind = ObjectKind::Future,
              .nullable = true,
              .signature = &payload,
          },
  };
  const auto result = outer.FlattenedResult();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, (TypeConstraint{.nullable = true}));
  EXPECT_TRUE(Heap::IsValueOfType(Value::Null(), *result));
}

}  // namespace
}  // namespace vela::runtime
