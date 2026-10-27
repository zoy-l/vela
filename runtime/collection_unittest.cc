#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

#include "base/execution_status.h"
#include "gtest/gtest.h"

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

class OrderedCollectionTest : public testing::Test {
 protected:
  enum class EntryPass : std::uint8_t { Initial, Reinserted };
  static constexpr std::uint32_t kEntryCount = 512;

  void SetUp() override {
    const auto* layout = isolate_.CreateCollectionLayout({
        .keys = {.element = {.kind = ValueKind::Integer}},
        .values = ArrayLayout{.element = {.kind = ValueKind::Integer}},
    });
    values_.at(0) = Value::Object(isolate_.heap().AllocateCollection(*layout));
    ASSERT_NE(values_.at(0).AsObject(), nullptr);
  }

  HeapObject& map() { return *values_.at(0).AsObject(); }
  Heap& heap() { return isolate_.heap(); }

  void InsertEntries(EntryPass pass) {
    const std::uint32_t stride = pass == EntryPass::Initial ? 1 : 2;
    const std::int64_t offset = pass == EntryPass::Initial ? 1000 : 2000;
    auto& heap = isolate_.heap();
    for (std::uint32_t index = 0; index < kEntryCount; index += stride) {
      ASSERT_EQ(heap.InsertCollection(map(), Value::Integer(index), Value::Integer(index + offset)),
                CollectionInsert::Inserted);
    }
  }

  void RemoveEvenEntries() {
    for (std::uint32_t index = 0; index < kEntryCount; index += 2) {
      ASSERT_EQ(Heap::RemoveCollection(map(), Value::Integer(index)), CollectionRemove::Removed);
    }
  }

  void VerifySnapshotOrder() {
    constexpr auto kHalfEntries = kEntryCount / 2;
    auto& heap = isolate_.heap();
    const auto* integers = isolate_.CreateArrayLayout({.element = {.kind = ValueKind::Integer}});
    values_.at(1) = Value::Object(heap.AllocateArray(*integers, kEntryCount));
    ASSERT_TRUE(heap.CopyCollection(map(), *values_.at(1).AsObject(), CollectionProjection::Keys));
    for (std::uint32_t index = 0; index < kHalfEntries; ++index) {
      EXPECT_EQ(Heap::GetElement(*values_.at(1).AsObject(), index)->AsInteger(), (index * 2) + 1);
      EXPECT_EQ(Heap::GetElement(*values_.at(1).AsObject(), index + kHalfEntries)->AsInteger(),
                index * 2);
    }
  }

 private:
  Isolate isolate_{1};
  std::array<Value, 2> values_;
  RootRange roots_{isolate_.heap(), values_.data(), values_.size()};
};

TEST_F(OrderedCollectionTest, PreservesOrderAcrossGrowthDeletionAndReinsertion) {
  InsertEntries(EntryPass::Initial);
  RemoveEvenEntries();
  InsertEntries(EntryPass::Reinserted);
  ASSERT_FALSE(HasFatalFailure());
  ASSERT_EQ(heap().InsertCollection(map(), Value::Integer(1), Value::Integer(42)),
            CollectionInsert::Updated);
  EXPECT_EQ(Heap::GetCollectionValue(map(), Value::Integer(1))->AsInteger(), 42);
  VerifySnapshotOrder();
}

TEST_F(OrderedCollectionTest, IteratorRejectsGrowthBeforeReadingCachedStorage) {
  InsertEntries(EntryPass::Initial);
  ASSERT_FALSE(HasFatalFailure());
  auto iterator = Heap::IterateCollection(map());
  ASSERT_TRUE(iterator);
  ASSERT_TRUE(heap().InsertCollection(map(), Value::Integer(kEntryCount), Value::Integer(42)));
  EXPECT_EQ(iterator->Next().error(), ExecutionStatus::ConcurrentModification);
}

TEST(CollectionTest, IteratorSeesValueUpdates) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* layout = isolate.CreateCollectionLayout({
      .keys = {.element = {.kind = ValueKind::Integer}},
      .values = ArrayLayout{.element = {.kind = ValueKind::Integer}},
  });
  const auto value = Value::Object(heap.AllocateCollection(*layout));
  RootRange root(heap, &value, 1);
  auto& map = *value.AsObject();
  ASSERT_TRUE(heap.InsertCollection(map, Value::Integer(1), Value::Integer(10)));
  auto iterator = Heap::IterateCollection(map);
  ASSERT_TRUE(iterator);
  ASSERT_EQ(heap.InsertCollection(map, Value::Integer(1), Value::Integer(42)),
            CollectionInsert::Updated);
  const auto entry = iterator->Next();
  ASSERT_TRUE(entry);
  const auto current = entry.value();
  if (!current) {
    FAIL() << "iterator ended before returning the updated entry";
    return;
  }
  EXPECT_EQ(current->key.AsInteger(), 1);
  EXPECT_EQ(current->value.AsInteger(), 42);
}

TEST_F(OrderedCollectionTest, IteratorRejectsStructuralChangesAfterEnd) {
  auto iterator = Heap::IterateCollection(map());
  ASSERT_TRUE(iterator);
  const auto end = iterator->Next();
  ASSERT_TRUE(end);
  EXPECT_FALSE(*end);
  ASSERT_TRUE(heap().InsertCollection(map(), Value::Integer(1), Value::Integer(10)));
  EXPECT_EQ(iterator->Next().error(), ExecutionStatus::ConcurrentModification);
}

TEST(CollectionTest, ClearsEntriesAndAllowsReuse) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* layout = isolate.CreateCollectionLayout({
      .keys = {.element = {.kind = ValueKind::Integer}},
      .values = ArrayLayout{.element = {.kind = ValueKind::Integer}},
  });
  Value value = Value::Object(heap.AllocateCollection(*layout));
  RootRange root(heap, &value, 1);
  auto& map = *value.AsObject();
  ASSERT_TRUE(heap.InsertCollection(map, Value::Integer(1), Value::Integer(42)));
  ASSERT_TRUE(Heap::ClearCollection(map));
  EXPECT_EQ(Heap::CollectionLength(map), 0U);
  EXPECT_FALSE(*Heap::HasCollectionKey(map, Value::Integer(1)));
  EXPECT_EQ(Heap::RemoveCollection(map, Value::Integer(1)), CollectionRemove::Missing);
  ASSERT_EQ(heap.InsertCollection(map, Value::Integer(42), Value::Integer(42)),
            CollectionInsert::Inserted);
  EXPECT_EQ(Heap::CollectionLength(map), 1U);
}

TEST(CollectionTest, CanonicalizesNanAndSignedZeroKeyEquality) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* layout =
      isolate.CreateCollectionLayout({.keys = {.element = {.kind = ValueKind::Double}}});
  Value value = Value::Object(heap.AllocateCollection(*layout));
  RootRange root(heap, &value, 1);
  auto& set = *value.AsObject();
  EXPECT_EQ(heap.InsertCollection(set, Value::Double(0.0)), CollectionInsert::Inserted);
  EXPECT_EQ(heap.InsertCollection(set, Value::Double(-0.0)), CollectionInsert::Updated);
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(heap.InsertCollection(set, Value::Double(nan)), CollectionInsert::Inserted);
  EXPECT_EQ(heap.InsertCollection(set, Value::Double(-nan)), CollectionInsert::Updated);
  EXPECT_EQ(Heap::CollectionLength(set), 2U);
  EXPECT_TRUE(*Heap::HasCollectionKey(set, Value::Double(nan)));
  EXPECT_EQ(Heap::RemoveCollection(set, Value::Double(-0.0)), CollectionRemove::Removed);
  EXPECT_FALSE(*Heap::HasCollectionKey(set, Value::Double(0.0)));
}

TEST(CollectionTest, TracesLiveEntriesAndReclaimsRemovedCyclesAndBuffers) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  const std::array<std::uint32_t, 1> references = {0};
  auto* instance = isolate.CreateClassLayout(1, references);
  const auto* layout = isolate.CreateCollectionLayout({
      .keys = {.element = {.kind = ValueKind::Object, .instance = instance}},
      .values = ArrayLayout{.element = {.kind = ValueKind::String}},
  });
  std::array<Value, 3> values;
  RootRange roots(heap, values.data(), values.size());
  values.at(0) = Value::Object(heap.AllocateCollection(*layout));
  values.at(1) = Value::Object(heap.AllocateInstance(*instance));
  values.at(2) = Value::String(heap.AllocateString("payload"));
  ASSERT_TRUE(heap.SetField(*values.at(1).AsObject(), 0, values.at(0)));
  ASSERT_TRUE(heap.InsertCollection(*values.at(0).AsObject(), values.at(1), values.at(2)));
  auto key = values.at(1);
  values.at(1) = Value::Null();
  values.at(2) = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 3U);
  ASSERT_EQ(Heap::RemoveCollection(*values.at(0).AsObject(), key), CollectionRemove::Removed);
  key = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
  values.at(0) = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
}

class CollectionBarrierTest : public testing::TestWithParam<GcPhase> {};

class SetCombinationTest : public testing::TestWithParam<SetOperation> {};

constexpr std::uint32_t kCombinationKeyCount = 64;
void InsertOverlappingStringKeys(Heap& heap, std::span<Value> values) {
  auto& left = *values.front().AsObject();
  auto& right = *values.subspan(1).front().AsObject();
  auto& key = values.subspan(2).front();
  for (std::uint32_t index = 0; index < kCombinationKeyCount; ++index) {
    key = Value::String(heap.AllocateString(std::to_string(index)));
    ASSERT_TRUE(heap.InsertCollection(left, key));
    key = Value::String(heap.AllocateString(std::to_string(index + (kCombinationKeyCount / 2))));
    ASSERT_TRUE(heap.InsertCollection(right, key));
  }
}

void DropInputSets(std::span<Value> values) {
  for (auto& value : values.first(2)) {
    ASSERT_TRUE(Heap::ClearCollection(*value.AsObject()));
    value = Value::Null();
  }
  values.subspan(2).front() = Value::Null();
}
TEST_P(SetCombinationTest, KeepsReferenceKeysAliveAcrossGrowthAndReturnsIndependentStorage) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  const auto* layout =
      isolate.CreateCollectionLayout({.keys = {.element = {.kind = ValueKind::String}}});
  std::array<Value, 4> values;
  RootRange roots(heap, values.data(), values.size());
  values.at(0) = Value::Object(heap.AllocateCollection(*layout));
  values.at(1) = Value::Object(heap.AllocateCollection(*layout));
  InsertOverlappingStringKeys(heap, values);
  const auto operation = GetParam();
  const auto combined =
      heap.CombineSets(*values.at(0).AsObject(), *values.at(1).AsObject(), operation);
  ASSERT_TRUE(combined);
  values.at(3) = Value::Object(*combined);
  const auto length = operation == SetOperation::Union
                          ? kCombinationKeyCount + (kCombinationKeyCount / 2)
                          : kCombinationKeyCount / 2;
  EXPECT_EQ(Heap::CollectionLength(**combined), length);
  DropInputSets(values);
  heap.FinishCollection();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), length + 1);
  values.at(2) =
      Value::String(heap.AllocateString(operation == SetOperation::Intersection ? "32" : "0"));
  EXPECT_EQ(Heap::HasCollectionKey(**combined, values.at(2)), true);
}
INSTANTIATE_TEST_SUITE_P(OrderedReferenceKeys,
                         SetCombinationTest,
                         testing::Values(SetOperation::Union,
                                         SetOperation::Intersection,
                                         SetOperation::Difference));

TEST_P(CollectionBarrierTest, ProtectsNewEdgesDuringIncrementalCollection) {
  const auto phase = GetParam();
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* layout =
      isolate.CreateCollectionLayout({.keys = {.element = {.kind = ValueKind::String}}});
  std::array<Value, 2> values;
  RootRange roots(heap, values.data(), values.size());
  values.at(0) = Value::Object(heap.AllocateCollection(*layout));
  values.at(1) = Value::String(heap.AllocateString("seed"));
  auto& set = *values.at(0).AsObject();
  ASSERT_TRUE(heap.InsertCollection(set, values.at(1)));
  ASSERT_TRUE(Heap::ClearCollection(set));
  values.at(1) = Value::Null();
  const auto candidate = Value::String(heap.AllocateString("survivor"));
  ASSERT_NE(heap.AllocateString("garbage"), nullptr);
  heap.StartCollection();
  heap.Step(1);
  if (phase == GcPhase::Sweep) {
    heap.Step(1);
  }
  ASSERT_EQ(heap.phase(), phase);
  ASSERT_TRUE(heap.InsertCollection(set, candidate));
  heap.FinishCollection();
  EXPECT_TRUE(*Heap::HasCollectionKey(set, candidate));
}

INSTANTIATE_TEST_SUITE_P(MarkingAndSweeping,
                         CollectionBarrierTest,
                         testing::Values(GcPhase::Mark, GcPhase::Sweep));

}  // namespace
}  // namespace vela::runtime
