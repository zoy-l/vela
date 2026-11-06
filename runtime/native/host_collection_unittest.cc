#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>

#include <gtest/gtest.h>
#include <vela/host.h>

#include "base/execution_status.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"

namespace vela {
namespace {

TEST(HostCollectionTest, RemovedChildIsRootedInTheDestinationScopeBeforeItsOwnerDisappears) {
  runtime::Isolate isolate(1);
  {
    HandleScope outer(isolate);
    std::optional<Local<Array<String>>> removed;
    {
      HandleScope inner(isolate);
      const auto values = CreateArray<Nullable<Array<String>>>(inner);
      const auto child = CreateArray<String>(inner);
      const auto text = CreateString(inner, "rooted child");
      ASSERT_TRUE(values.has_value());
      ASSERT_TRUE(child.has_value());
      ASSERT_TRUE(text.has_value());
      ASSERT_TRUE(child->Push(inner, *text));
      ASSERT_TRUE(values->Push(inner, *child));
      ASSERT_TRUE(values->Push(inner, std::nullopt));
      const auto null = values->Pop(outer);
      ASSERT_TRUE(null.has_value());
      if (!*null) {
        FAIL() << "expected a removed null element";
      }
      EXPECT_FALSE(**null);
      const auto result = values->Shift(outer);
      ASSERT_TRUE(result.has_value());
      if (!*result || !**result) {
        FAIL() << "expected the removed child array";
      }
      removed = ***result;
      const auto empty = values->Pop(outer);
      ASSERT_TRUE(empty.has_value());
      EXPECT_FALSE(*empty);
    }
    isolate.heap().Collect();
    ASSERT_TRUE(removed.has_value());
    const auto text = removed->Get(outer, 0);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->Utf8(), "rooted child");
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

TEST(HostCollectionTest, ArrayMutationsPreserveRelativeRangesAndSelfInsertion) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto values = CreateArray<std::int64_t>(scope);
  ASSERT_TRUE(values.has_value());
  EXPECT_EQ(values->Size(), 0);
  ASSERT_TRUE(values->Push(scope, 1));
  ASSERT_TRUE(values->Push(scope, 2));
  ASSERT_TRUE(values->Push(scope, 3));
  const auto removed = values->Splice(scope, -2, 1, *values);
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(removed->Size(), 1);
  EXPECT_EQ(*removed->Get(scope, 0), 2);
  EXPECT_EQ(values->Size(), 5);
  ASSERT_TRUE(values->Reverse(scope));
  const auto slice = values->Slice(scope, -3, 5);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->Size(), 3);
  EXPECT_EQ(*slice->Get(scope, 0), 2);
  ASSERT_TRUE(slice->Fill(scope, 7, 0, -1));
  EXPECT_EQ(*slice->Get(scope, 0), 7);
  EXPECT_EQ(*slice->Get(scope, 1), 7);
  EXPECT_EQ(*slice->Get(scope, 2), 1);
  ASSERT_TRUE(values->Unshift(scope, *slice));
  ASSERT_TRUE(values->PushMany(scope, *slice));
  EXPECT_EQ(values->Size(), 11);
  EXPECT_EQ(*values->Get(scope, 0), 7);
  EXPECT_EQ(*values->Get(scope, 10), 1);
  ASSERT_TRUE(values->Clear(scope));
  EXPECT_EQ(values->Size(), 0);
}

TEST(HostCollectionTest, ArrayBoundsAndGrowthFailuresPreserveExistingElements) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto values = CreateArray<std::int64_t>(scope);
  ASSERT_TRUE(values.has_value());
  ASSERT_TRUE(values->Push(scope, 42));
  const auto missing = values->Get(scope, 1);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().status, ExecutionStatus::IndexOutOfBounds);
  const auto invalid = values->Reserve(scope, -1);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().status, ExecutionStatus::IndexOutOfBounds);
  auto& heap = isolate.heap();
  heap.SetAllocationLimit(heap.allocated_bytes());
  const auto failed = values->Reserve(scope, values->Capacity() + 1);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().status, ExecutionStatus::OutOfMemory);
  EXPECT_EQ(values->Size(), 1);
  EXPECT_EQ(*values->Get(scope, 0), 42);
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
}

TEST(HostCollectionTest, MapDistinguishesMissingFromNullAndKeepsInsertionOrder) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto values = CreateMap<std::int64_t, Nullable<String>>(scope);
  ASSERT_TRUE(values.has_value());
  {
    HandleScope inner(isolate);
    const auto text = CreateString(inner, "value");
    ASSERT_TRUE(text.has_value());
    ASSERT_TRUE(values->Set(inner, 1, std::nullopt));
    ASSERT_TRUE(values->Set(inner, 2, *text));
  }
  const auto missing = values->Get(scope, 3);
  ASSERT_TRUE(missing.has_value());
  EXPECT_FALSE(*missing);
  const auto null = values->Get(scope, 1);
  ASSERT_TRUE(null.has_value());
  if (!*null) {
    FAIL() << "expected a stored null value";
  }
  EXPECT_FALSE(**null);
  const auto found = values->Get(scope, 2);
  ASSERT_TRUE(found.has_value());
  if (!*found || !**found) {
    FAIL() << "expected the stored string value";
  }
  const auto found_text = ***found;
  EXPECT_EQ(found_text.Utf8(), "value");
  ASSERT_EQ(*values->Delete(scope, 2), Removal::Removed);
  ASSERT_EQ(*values->Delete(scope, 2), Removal::Missing);
  isolate.heap().Collect();
  EXPECT_EQ(found_text.Utf8(), "value");
  ASSERT_TRUE(values->Set(scope, 2, found_text));
  const auto keys = values->Keys(scope);
  const auto projected = values->Values(scope);
  ASSERT_TRUE(keys.has_value());
  ASSERT_TRUE(projected.has_value());
  EXPECT_EQ(keys->Size(), 2);
  EXPECT_EQ(*keys->Get(scope, 0), 1);
  EXPECT_EQ(*keys->Get(scope, 1), 2);
  ASSERT_TRUE(values->Clear(scope));
  isolate.heap().Collect();
  const auto text = projected->Get(scope, 1);
  ASSERT_TRUE(text.has_value());
  if (!*text) {
    FAIL() << "expected the projected string value";
  }
  EXPECT_EQ((**text).Utf8(), "value");
}

TEST(HostCollectionTest, SetUsesObjectIdentityAndSharedSetOperations) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto first = CreateArray<std::int64_t>(scope);
  const auto second = CreateArray<std::int64_t>(scope);
  const auto left = CreateSet<Array<std::int64_t>>(scope);
  const auto right = CreateSet<Array<std::int64_t>>(scope);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(right.has_value());
  EXPECT_EQ(*left->Add(scope, *first), Insertion::Inserted);
  EXPECT_EQ(*left->Add(scope, *first), Insertion::Existing);
  EXPECT_EQ(*left->Add(scope, *second), Insertion::Inserted);
  EXPECT_EQ(left->Size(), 2);
  ASSERT_TRUE(right->Add(scope, *second));
  const auto common = left->Intersection(scope, *right);
  const auto difference = left->Difference(scope, *right);
  const auto all = left->Union(scope, *right);
  ASSERT_TRUE(common.has_value());
  ASSERT_TRUE(difference.has_value());
  ASSERT_TRUE(all.has_value());
  EXPECT_EQ(common->Size(), 1);
  EXPECT_TRUE(*common->Has(scope, *second));
  EXPECT_FALSE(*common->Has(scope, *first));
  EXPECT_TRUE(*difference->Has(scope, *first));
  EXPECT_EQ(all->Size(), 2);
  const auto keys = left->Keys(scope);
  ASSERT_TRUE(keys.has_value());
  const auto key = keys->Get(scope, 0);
  ASSERT_TRUE(key.has_value());
  ASSERT_TRUE(key->Push(scope, 42));
  EXPECT_EQ(*first->Get(scope, 0), 42);
}

TEST(HostCollectionTest, CollectionKeysPreserveLanguageNumericEquality) {
  runtime::Isolate isolate;
  HandleScope scope(isolate);
  const auto values = CreateSet<double>(scope);
  ASSERT_TRUE(values.has_value());
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(*values->Add(scope, nan), Insertion::Inserted);
  EXPECT_EQ(*values->Add(scope, nan), Insertion::Existing);
  EXPECT_EQ(*values->Add(scope, -0.0), Insertion::Inserted);
  EXPECT_EQ(*values->Add(scope, 0.0), Insertion::Existing);
  EXPECT_EQ(values->Size(), 2);
}

TEST(HostCollectionTest, NestedNullableElementsUseOneOptionalAtTheHostBoundary) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto values = CreateArray<Nullable<Nullable<String>>>(scope);
  const auto text = CreateString(scope, "flattened");
  ASSERT_TRUE(values.has_value());
  ASSERT_TRUE(text.has_value());
  ASSERT_TRUE(values->Push(scope, *text));
  ASSERT_TRUE(values->Push(scope, std::nullopt));
  const auto present = values->Get(scope, 0);
  const auto null = values->Get(scope, 1);
  ASSERT_TRUE(present.has_value());
  ASSERT_TRUE(null.has_value());
  ASSERT_TRUE(present->has_value());
  EXPECT_EQ((**present).Utf8(), "flattened");
  EXPECT_FALSE(*null);
}

TEST(HostTypeTest, TypeCopiesShareOneCloseBlockerAndReleaseTheirNativeStorage) {
  runtime::Isolate isolate(1);
  const auto baseline = isolate.heap().allocated_bytes();
  {
    auto first = Type<Array<String>>::Bind(isolate);
    ASSERT_TRUE(first.has_value());
    const auto copied = *first;
    first = std::unexpected(Error{.message = {}, .stack_trace = {}});
    EXPECT_EQ(&copied.isolate(), &isolate);
    const auto blockers = isolate.ReloadBlockers();
    ASSERT_EQ(blockers.size(), 1U);
    EXPECT_EQ(blockers.front().kind, runtime::ExternalResourceKind::Type);
    EXPECT_FALSE(isolate.TryStop());
  }
  EXPECT_EQ(isolate.heap().allocated_bytes(), baseline);
  EXPECT_TRUE(isolate.TryStop());
}

TEST(HostTypeTest, TypeAllocationFailureDoesNotLeaveACloseBlocker) {
  runtime::Isolate isolate;
  isolate.heap().SetAllocationLimit(0);
  const auto bound = Type<String>::Bind(isolate);
  ASSERT_FALSE(bound);
  EXPECT_EQ(bound.error().status, ExecutionStatus::OutOfMemory);
  EXPECT_TRUE(isolate.ReloadBlockers().empty());
  EXPECT_TRUE(isolate.TryStop());
}

TEST(HostTypeTest, CastAndCollectionOperationsRejectForeignIsolates) {
  runtime::Isolate first;
  runtime::Isolate second;
  HandleScope scope(first);
  HandleScope foreign(second);
  const auto type = Type<Array<String>>::Bind(first);
  const auto array = CreateArray<String>(scope);
  const auto text = CreateString(foreign, "foreign");
  ASSERT_TRUE(type.has_value());
  ASSERT_TRUE(array.has_value());
  ASSERT_TRUE(text.has_value());
  const auto creation = CreateArray(foreign, *type);
  ASSERT_FALSE(creation);
  EXPECT_EQ(creation.error().status, ExecutionStatus::InvalidIsolate);
  const auto insertion = array->Push(scope, *text);
  ASSERT_FALSE(insertion);
  EXPECT_EQ(insertion.error().status, ExecutionStatus::InvalidIsolate);
  const auto cast = type->Cast(foreign, array->ToValue());
  ASSERT_FALSE(cast);
  EXPECT_EQ(cast.error().status, ExecutionStatus::InvalidIsolate);
  const auto foreign_value = type->Cast(scope, text->ToValue());
  ASSERT_FALSE(foreign_value);
  EXPECT_EQ(foreign_value.error().status, ExecutionStatus::InvalidIsolate);
  const auto integer = Type<std::int64_t>::Bind(first);
  ASSERT_TRUE(integer.has_value());
  const auto invalid = integer->Cast(scope, array->ToValue());
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().status, ExecutionStatus::InvalidCast);
}

}  // namespace
}  // namespace vela
