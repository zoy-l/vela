#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

#include "base/execution_status.h"
#include "gtest/gtest.h"

#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "vela/types.h"

namespace vela::runtime {
namespace {

class OrderedCollectionTest : public testing::Test {
 protected:
  static constexpr std::uint32_t kEntryCount = 512;

  void SetUp() override {
    const auto* layout = isolate_.CreateCollectionLayout({
        .keys = {.element = {.kind = ValueKind::Integer}},
        .values = ArrayLayout{.element = {.kind = ValueKind::Integer}},
    });
    map_ = Value::Object(isolate_.heap().AllocateCollection(*layout));
    ASSERT_NE(map_.AsObject(), nullptr);
  }

  HeapObject& map() { return *map_.AsObject(); }
  Heap& heap() { return isolate_.heap(); }

  void ExpectInteger(Value key, std::int64_t expected) {
    const auto value = Heap::GetCollectionValue(map(), key);
    ASSERT_TRUE(value);
    EXPECT_EQ(value->AsInteger(), expected);
  }

  void InsertEntries(std::uint32_t stride = 1) {
    auto& heap = isolate_.heap();
    auto& collection = map();
    for (std::uint32_t index = 0; index < kEntryCount; index += stride) {
      ASSERT_EQ(heap.InsertCollection(collection, Value::Integer(index),
                                      Value::Integer(index + 1000)),
                Insertion::Inserted);
    }
  }

  void VerifySnapshotOrder() {
    constexpr auto kHalfEntries = kEntryCount / 2;
    auto& heap = isolate_.heap();
    const auto* integers = isolate_.CreateArrayLayout({.element = {.kind = ValueKind::Integer}});
    const auto value = Value::Object(heap.AllocateArray(*integers, kEntryCount));
    const RootRange roots(heap, &value, 1);
    auto& snapshot = *value.AsObject();
    ASSERT_TRUE(heap.CopyCollection(map(), snapshot, CollectionProjection::Keys));
    for (std::uint32_t index = 0; index < kHalfEntries; ++index) {
      EXPECT_EQ(heap.GetElement(snapshot, index)->AsInteger(), (index * 2) + 1);
      EXPECT_EQ(heap.GetElement(snapshot, index + kHalfEntries)->AsInteger(), index * 2);
    }
  }

 private:
  Isolate isolate_{1};
  Value map_;
  RootRange root_{isolate_.heap(), &map_, 1};
};

TEST_F(OrderedCollectionTest, PreservesOrderAcrossGrowthDeletionAndReinsertion) {
  ASSERT_NO_FATAL_FAILURE(InsertEntries());
  auto& collection = map();
  for (std::uint32_t index = 0; index < kEntryCount; index += 2) {
    ASSERT_EQ(Heap::RemoveCollection(collection, Value::Integer(index)), Removal::Removed);
  }
  ASSERT_NO_FATAL_FAILURE(InsertEntries(2));
  ASSERT_EQ(heap().InsertCollection(collection, Value::Integer(1), Value::Integer(42)),
            Insertion::Existing);
  ASSERT_NO_FATAL_FAILURE(ExpectInteger(Value::Integer(1), 42));
  VerifySnapshotOrder();
}

TEST(OrderedStringMapTest, LocatedUpdatesPreserveEqualStringsNullableValuesAndGrowthUnderGc) {
  for (const auto nullable : std::array{false, true}) {
    Isolate isolate(1);
    auto& heap = isolate.heap();
    const auto* layout = isolate.CreateCollectionLayout({
        .keys = {.element = {.kind = ValueKind::String}},
        .values = ArrayLayout{.element = {.kind = ValueKind::Integer, .nullable = nullable}},
    });
    std::array<Value, 2> roots;
    const RootRange range(heap, roots.data(), roots.size());
    roots[0] = Value::Object(heap.AllocateCollection(*layout));
    auto* map = roots[0].AsObject();
    ASSERT_NE(map, nullptr);
    constexpr std::int64_t kKeys = 257;
    constexpr std::int64_t kRounds = 4;
    for (std::int64_t index = 0; index < kKeys * kRounds; ++index) {
      // 每次分配不同对象，重复键必须按内容命中，不能只比较字符串指针。
      roots[1] = Value::String(heap.AllocateString(std::to_string(index % kKeys)));
      auto* key = roots[1].AsString();
      ASSERT_NE(key, nullptr);
      Heap::MapLookup lookup{};
      const auto previous = Heap::ResolveStringMapValue(*map, key, lookup);
      EXPECT_EQ(previous.IsNull(), index < kKeys);
      const auto count = previous.IsNull() ? 0 : previous.AsInteger();
      EXPECT_EQ(count, index / kKeys);
      ASSERT_TRUE(heap.StoreMapValue(*map, roots[1], Value::Integer(count + 1), lookup));
      heap.Collect();
    }
    if (nullable) {
      ASSERT_TRUE(heap.StoreMapValue(*map, roots[1], Value::Null()));
      Heap::MapLookup lookup{};
      EXPECT_TRUE(Heap::ResolveStringMapValue(*map, roots[1].AsString(), lookup).IsNull());
      EXPECT_EQ(lookup.integer_value, nullptr);
    }
  }
}

TEST_F(OrderedCollectionTest, IteratorRejectsGrowthBeforeReadingCachedStorage) {
  ASSERT_NO_FATAL_FAILURE(InsertEntries());
  auto iterator = Heap::IterateCollection(map());
  ASSERT_TRUE(iterator);
  ASSERT_TRUE(heap().InsertCollection(map(), Value::Integer(kEntryCount), Value::Integer(42)));
  EXPECT_EQ(iterator->Next().error(), ExecutionStatus::ConcurrentModification);
}

TEST_F(OrderedCollectionTest, IteratorSeesValueUpdates) {
  ASSERT_TRUE(heap().InsertCollection(map(), Value::Integer(1), Value::Integer(10)));
  auto iterator = Heap::IterateCollection(map());
  ASSERT_TRUE(iterator);
  ASSERT_EQ(heap().InsertCollection(map(), Value::Integer(1), Value::Integer(42)),
            Insertion::Existing);
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

TEST_F(OrderedCollectionTest, TypedUpdatesUseCurrentKeysAfterDeletionClearGrowthAndGc) {
  ASSERT_NO_FATAL_FAILURE(InsertEntries());
  const auto first = Value::Integer(1);
  const auto second = Value::Integer(2);
  EXPECT_EQ(Heap::LoadMapValue(map(), first).AsInteger(), 1001);
  ASSERT_TRUE(heap().StoreMapValue(map(), second, Value::Integer(42)));
  ASSERT_NO_FATAL_FAILURE(ExpectInteger(first, 1001));
  ASSERT_NO_FATAL_FAILURE(ExpectInteger(second, 42));

  EXPECT_EQ(Heap::LoadMapValue(map(), first).AsInteger(), 1001);
  ASSERT_EQ(Heap::RemoveCollection(map(), first), Removal::Removed);
  ASSERT_TRUE(heap().StoreMapValue(map(), first, Value::Integer(50)));
  ASSERT_NO_FATAL_FAILURE(ExpectInteger(first, 50));
  ASSERT_NO_FATAL_FAILURE(ExpectInteger(second, 42));

  EXPECT_EQ(Heap::LoadMapValue(map(), second).AsInteger(), 42);
  ASSERT_TRUE(Heap::ClearCollection(map()));
  ASSERT_TRUE(heap().StoreMapValue(map(), second, Value::Integer(60)));
  EXPECT_TRUE(Heap::GetCollectionValue(map(), first)->IsNull());
  ASSERT_NO_FATAL_FAILURE(ExpectInteger(second, 60));

  EXPECT_EQ(Heap::LoadMapValue(map(), second).AsInteger(), 60);
  for (std::uint32_t index = 0; index < kEntryCount * 2; ++index) {
    ASSERT_TRUE(
        heap().InsertCollection(map(), Value::Integer(index + 1000), Value::Integer(index)));
  }
  ASSERT_TRUE(heap().StoreMapValue(map(), second, Value::Integer(70)));
  ASSERT_NO_FATAL_FAILURE(ExpectInteger(second, 70));
  ASSERT_NO_FATAL_FAILURE(ExpectInteger(Value::Integer(1000), 0));

  EXPECT_EQ(Heap::LoadMapValue(map(), second).AsInteger(), 70);
  heap().Collect();
  ASSERT_TRUE(heap().StoreMapValue(map(), second, Value::Integer(80)));
  ASSERT_NO_FATAL_FAILURE(ExpectInteger(second, 80));
  EXPECT_TRUE(Heap::LoadMapValue(map(), first).IsNull());
  ASSERT_TRUE(heap().StoreMapValue(map(), first, Value::Integer(90)));
  ASSERT_NO_FATAL_FAILURE(ExpectInteger(first, 90));
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

TEST_F(OrderedCollectionTest, ClearsEntriesAndAllowsReuse) {
  ASSERT_TRUE(heap().InsertCollection(map(), Value::Integer(1), Value::Integer(42)));
  ASSERT_TRUE(Heap::ClearCollection(map()));
  EXPECT_EQ(Heap::CollectionLength(map()), 0U);
  EXPECT_FALSE(*Heap::HasCollectionKey(map(), Value::Integer(1)));
  EXPECT_EQ(Heap::RemoveCollection(map(), Value::Integer(1)), Removal::Missing);
  ASSERT_EQ(heap().InsertCollection(map(), Value::Integer(42), Value::Integer(42)),
            Insertion::Inserted);
  EXPECT_EQ(Heap::CollectionLength(map()), 1U);
}

TEST_F(OrderedCollectionTest, ReusesDeletedBucketsAcrossRepeatedEmptyTransitions) {
  ASSERT_NO_FATAL_FAILURE(InsertEntries());
  for (std::uint32_t index = 0; index < kEntryCount; ++index) {
    ASSERT_EQ(Heap::RemoveCollection(map(), Value::Integer(index)), Removal::Removed);
  }
  for (std::uint32_t index = 0; index < kEntryCount * 4; ++index) {
    const auto key = Value::Integer(index);
    ASSERT_TRUE(heap().StoreMapValue(map(), key, Value::Integer(42)));
    EXPECT_EQ(Heap::LoadMapValue(map(), key).AsInteger(), 42);
    ASSERT_EQ(Heap::RemoveCollection(map(), key), Removal::Removed);
    EXPECT_EQ(Heap::CollectionLength(map()), 0U);
    EXPECT_TRUE(Heap::LoadMapValue(map(), key).IsNull());
  }
  heap().Collect();
  ASSERT_TRUE(heap().StoreMapValue(map(), Value::Integer(1), Value::Integer(42)));
  ASSERT_NO_FATAL_FAILURE(ExpectInteger(Value::Integer(1), 42));
}

TEST_F(OrderedCollectionTest, RejectsWrongKeyTypesBeforeEmptyAndPopulatedLookups) {
  auto& collection = map();
  const auto check_wrong_key = [&collection] {
    const auto wrong = Value::Boolean(true);
    EXPECT_EQ(Heap::HasCollectionKey(collection, wrong).error(), ExecutionStatus::TypeError);
    EXPECT_EQ(Heap::GetCollectionValue(collection, wrong).error(), ExecutionStatus::TypeError);
    EXPECT_EQ(Heap::RemoveCollection(collection, wrong).error(), ExecutionStatus::TypeError);
  };
  check_wrong_key();
  const auto key = Value::Integer(1);
  ASSERT_TRUE(heap().InsertCollection(collection, key, Value::Integer(42)));
  check_wrong_key();
  EXPECT_EQ(Heap::CollectionLength(collection), 1U);
  ASSERT_NO_FATAL_FAILURE(ExpectInteger(key, 42));
}

TEST(CollectionTest, CanonicalizesNanAndSignedZeroKeyEquality) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* layout =
      isolate.CreateCollectionLayout({.keys = {.element = {.kind = ValueKind::Double}}});
  Value value = Value::Object(heap.AllocateCollection(*layout));
  RootRange root(heap, &value, 1);
  auto& set = *value.AsObject();
  EXPECT_EQ(heap.InsertCollection(set, Value::Double(0.0)), Insertion::Inserted);
  EXPECT_EQ(heap.InsertCollection(set, Value::Double(-0.0)), Insertion::Existing);
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(heap.InsertCollection(set, Value::Double(nan)), Insertion::Inserted);
  EXPECT_EQ(heap.InsertCollection(set, Value::Double(-nan)), Insertion::Existing);
  EXPECT_EQ(Heap::CollectionLength(set), 2U);
  EXPECT_TRUE(*Heap::HasCollectionKey(set, Value::Double(nan)));
  EXPECT_EQ(Heap::RemoveCollection(set, Value::Double(-0.0)), Removal::Removed);
  EXPECT_FALSE(*Heap::HasCollectionKey(set, Value::Double(0.0)));
}

TEST(CollectionTest, TracesLiveEntriesAndReclaimsRemovedCyclesAndBuffers) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  const std::array<std::uint32_t, 1> references = {0};
  auto* instance = isolate.CreateClassLayout(1, references);
  const auto* layout = isolate.CreateCollectionLayout({
      .keys = {.element = {.kind = ValueKind::Object, .nominal = &instance->nominal_type()}},
      .values = ArrayLayout{.element = {.kind = ValueKind::String}},
  });
  std::array<Value, 3> values;
  RootRange roots(heap, values.data(), values.size());
  values.at(0) = Value::Object(heap.AllocateCollection(*layout));
  values.at(1) = Value::Object(heap.AllocateInstance(*instance));
  values.at(2) = Value::String(heap.AllocateString("payload"));
  ASSERT_TRUE(heap.SetField(*values.at(1).AsObject(), 0, values.at(0)));
  ASSERT_TRUE(heap.InsertCollection(*values.at(0).AsObject(), values.at(1), values.at(2)));
  const auto key = values.at(1);
  values.at(1) = Value::Null();
  values.at(2) = Value::Null();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 3U);
  ASSERT_EQ(Heap::RemoveCollection(*values.at(0).AsObject(), key), Removal::Removed);
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
