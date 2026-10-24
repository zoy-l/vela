#include <array>
#include <cstdint>

#include "gtest/gtest.h"

#include "runtime/isolate.h"

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
  Value array = Value::Object(heap.AllocateArray(1));
  RootRange roots(heap, &array, 1);
  auto* child = heap.AllocateInstance(*layout);
  ASSERT_TRUE(heap.SetElement(*array.AsObject(), 0, Value::Object(child)));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  const Value* element = Heap::GetElement(*array.AsObject(), 0);
  ASSERT_NE(element, nullptr);
  ASSERT_TRUE(element->IsObject());
  EXPECT_EQ(element->AsObject(), child);
  array = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
  array = Value::Object(heap.AllocateArray(2));
  const auto* first = Heap::GetElement(*array.AsObject(), 0);
  const auto* second = Heap::GetElement(*array.AsObject(), 1);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_TRUE(first->IsNull());
  EXPECT_TRUE(second->IsNull());
}

}  // namespace
}  // namespace vela::runtime
