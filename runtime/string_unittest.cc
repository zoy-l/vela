#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

TEST(StringGcTest, StoresEmptyAndEmbeddedNullBytes) {
  Heap heap;
  auto* empty = heap.AllocateString("");
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty->value().empty());
  constexpr std::string_view kBytes("a\0b", 3);
  auto* joined = heap.AllocateString(kBytes, kBytes);
  ASSERT_NE(joined, nullptr);
  EXPECT_EQ(joined->value(), std::string_view("a\0ba\0b", 6));
  EXPECT_EQ(heap.object_count(), 2U);
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), 0U);
}

TEST(StringGcTest, AccountsAndReleasesLargePayload) {
  Heap heap;
  constexpr std::size_t kLength = 8192;
  const std::string text(kLength, 'x');
  Value value = Value::String(heap.AllocateString(text));
  RootRange root(heap, &value, 1);
  ASSERT_NE(value.AsString(), nullptr);
  EXPECT_EQ(heap.allocated_bytes(), sizeof(StringObject) + kLength);
  EXPECT_EQ(heap.committed_bytes(), heap.allocated_bytes());
  heap.Collect();
  EXPECT_EQ(value.AsString()->value(), text);
  value = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
  EXPECT_EQ(heap.committed_bytes(), 0U);
}

TEST(StringGcTest, ReusesBlocksAcrossStringAndInstanceLifetimes) {
  Isolate isolate;
  auto& heap = isolate.heap();
  auto* layout = isolate.CreateClassLayout(0, {});
  ASSERT_NE(heap.AllocateString("vela"), nullptr);
  const auto committed = heap.committed_bytes();
  heap.Collect();
  constexpr int kReuseIterations = 32;
  for (int iteration = 0; iteration < kReuseIterations; ++iteration) {
    ASSERT_NE(heap.AllocateString("vela"), nullptr);
    heap.Collect();
    ASSERT_NE(heap.AllocateInstance(*layout), nullptr);
    heap.Collect();
  }
  EXPECT_EQ(heap.committed_bytes(), committed);
}

TEST(StringGcTest, NativeConstantRootsRetainStringsAndDetach) {
  Heap heap(1);
  std::array<StringObject*, 3> constants{};
  {
    RootRange roots(heap, constants);
    for (auto& constant : constants) {
      constant = heap.AllocateString("constant");
      ASSERT_NE(constant, nullptr);
    }
    heap.Collect();
    EXPECT_EQ(heap.object_count(), constants.size());
    for (const auto* constant : constants) {
      EXPECT_EQ(constant->value(), "constant");
    }
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

TEST(StringGcTest, CollectsCycleContainingStringsAndArrays) {
  Isolate isolate;
  auto& heap = isolate.heap();
  constexpr std::array<std::uint32_t, 2> kReferences = {0, 1};
  auto* layout = isolate.CreateClassLayout(2, kReferences);
  const auto* arrays =
      isolate.CreateArrayLayout({.element_kind = ValueKind::Object, .element_class = layout});
  Value root = Value::Object(heap.AllocateInstance(*layout));
  RootRange roots(heap, &root, 1);
  auto* string = heap.AllocateString("inside cycle");
  ASSERT_TRUE(heap.SetField(*root.AsObject(), 0, Value::String(string)));
  auto* array = heap.AllocateArray(*arrays, 1);
  ASSERT_TRUE(heap.SetField(*root.AsObject(), 1, Value::Object(array)));
  ASSERT_TRUE(heap.SetElement(*array, 0, root));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 3U);
  EXPECT_EQ(Heap::GetField(*root.AsObject(), 0)->AsString()->value(), "inside cycle");
  root = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
}

class StringBarrierTest : public testing::TestWithParam<std::size_t> {};

TEST_P(StringBarrierTest, ArrayStoreMarksWhiteStringDuringMarkAndSweep) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* layout = isolate.CreateArrayLayout({.element_kind = ValueKind::String});
  auto* string = heap.AllocateString("retained");
  Value root = Value::Object(heap.AllocateArray(*layout, 1));
  RootRange roots(heap, &root, 1);
  heap.StartCollection();
  heap.Step(GetParam());
  EXPECT_EQ(heap.phase(), GetParam() == 1 ? GcPhase::Mark : GcPhase::Sweep);
  ASSERT_TRUE(heap.SetElement(*root.AsObject(), 0, Value::String(string)));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(Heap::GetElement(*root.AsObject(), 0)->AsString()->value(), "retained");
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
}

TEST_P(StringBarrierTest, FieldStoreMarksWhiteStringDuringMarkAndSweep) {
  Isolate isolate;
  auto& heap = isolate.heap();
  constexpr std::array<std::uint32_t, 1> kReferences = {0};
  auto* layout = isolate.CreateClassLayout(1, kReferences);
  auto* string = heap.AllocateString("retained");
  Value root = Value::Object(heap.AllocateInstance(*layout));
  RootRange roots(heap, &root, 1);
  heap.StartCollection();
  heap.Step(GetParam());
  ASSERT_TRUE(heap.SetField(*root.AsObject(), 0, Value::String(string)));
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(Heap::GetField(*root.AsObject(), 0)->AsString()->value(), "retained");
}

INSTANTIATE_TEST_SUITE_P(IncrementalPhases, StringBarrierTest, testing::Values(1U, 2U));

TEST(StringGcTest, RescansChangedRootBeforeSweep) {
  Heap heap;
  Value value = Value::Null();
  RootRange root(heap, &value, 1);
  auto* string = heap.AllocateString("moved to root");
  heap.StartCollection();
  value = Value::String(string);
  heap.FinishCollection();
  EXPECT_EQ(heap.object_count(), 1U);
  EXPECT_EQ(value.AsString()->value(), "moved to root");
}

TEST(StringGcTest, RegistersNativeRootsAndAllocatesDuringSweep) {
  Heap heap;
  std::array<StringObject*, 1> constants = {heap.AllocateString("unswept")};
  ASSERT_NE(heap.AllocateString("garbage"), nullptr);
  heap.StartCollection();
  heap.Step(1);
  ASSERT_EQ(heap.phase(), GcPhase::Sweep);
  RootRange roots(heap, constants);
  Value value = Value::String(heap.AllocateString("new string"));
  RootRange new_root(heap, &value, 1);
  heap.FinishCollection();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(constants.front()->value(), "unswept");
  EXPECT_EQ(value.AsString()->value(), "new string");
}

}  // namespace
}  // namespace vela::runtime
