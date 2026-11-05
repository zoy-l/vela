#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"

namespace vela {
namespace {

using internal::HandleAccess;

TEST(HandleScopeTest, GrowthPreservesEarlierLocalsAndReleasesAllStorage) {
  runtime::Isolate isolate(1);
  auto& heap = isolate.heap();
  const auto baseline = heap.allocated_bytes();
  {
    HandleScope scope(isolate);
    auto* text = heap.AllocateString("stable local");
    ASSERT_NE(text, nullptr);
    const auto first = HandleAccess::Root<String>(scope, runtime::Value::String(text));
    ASSERT_TRUE(first);
    std::vector<Local<String>> copies;
    constexpr std::size_t kCount = 100;
    for (std::size_t index = 0; index < kCount; ++index) {
      const auto copy = HandleAccess::Root<String>(scope, HandleAccess::Read(*first));
      ASSERT_TRUE(copy);
      copies.push_back(*copy);
    }
    heap.Collect();
    EXPECT_EQ(heap.object_count(), 1U);
    EXPECT_EQ(HandleAccess::Read(copies.front()).AsString()->value(), "stable local");
    EXPECT_EQ(HandleAccess::Read(copies.back()).AsString(), text);
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
  EXPECT_EQ(heap.allocated_bytes(), baseline);
}

TEST(HandleScopeTest, PersistenceSharesOneRootAndLocalizationSurvivesReset) {
  runtime::Isolate isolate(1);
  auto& heap = isolate.heap();
  const auto baseline = heap.allocated_bytes();
  Persistent<String> saved;
  {
    HandleScope scope(isolate);
    const auto local = HandleAccess::Root<String>(
        scope, runtime::Value::String(heap.AllocateString("persistent")));
    ASSERT_TRUE(local);
    auto persisted = local->Persist();
    ASSERT_TRUE(persisted);
    saved = std::move(*persisted);
  }
  heap.Collect();
  const auto rooted_bytes = heap.allocated_bytes();
  auto copy = saved;
  auto moved = std::move(copy);
  EXPECT_EQ(heap.allocated_bytes(), rooted_bytes);
  saved.Reset();
  {
    HandleScope scope(isolate);
    const auto local = moved.ToLocal(scope);
    ASSERT_TRUE(local);
    moved.Reset();
    heap.Collect();
    EXPECT_EQ(HandleAccess::Read(*local).AsString()->value(), "persistent");
    EXPECT_FALSE(isolate.TryStop());
  }
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), baseline);
  EXPECT_TRUE(isolate.TryStop());
}

TEST(HandleScopeTest, OuterScopeOwnsTransferredLocalAfterInnerScopeEnds) {
  runtime::Isolate isolate(1);
  HandleScope outer(isolate);
  std::optional<Local<String>> transferred;
  {
    HandleScope inner(isolate);
    const auto source = HandleAccess::Root<String>(
        inner, runtime::Value::String(isolate.heap().AllocateString("escaped")));
    ASSERT_TRUE(source);
    const auto destination = HandleAccess::Root<String>(outer, HandleAccess::Read(*source));
    ASSERT_TRUE(destination);
    transferred = *destination;
  }
  isolate.heap().Collect();
  EXPECT_EQ(HandleAccess::Read(*transferred).AsString()->value(), "escaped");
}

TEST(HandleScopeTest, FailedPersistenceAndGrowthKeepExistingRootsIntact) {
  runtime::Isolate isolate(1);
  auto& heap = isolate.heap();
  HandleScope scope(isolate);
  const auto first =
      HandleAccess::Root<String>(scope, runtime::Value::String(heap.AllocateString("budget")));
  ASSERT_TRUE(first);
  const auto baseline = heap.allocated_bytes();
  heap.SetAllocationLimit(baseline);
  const auto persisted = first->Persist();
  ASSERT_FALSE(persisted);
  EXPECT_EQ(persisted.error().status, ExecutionStatus::OutOfMemory);
  constexpr std::size_t kInlineCount = 16;
  for (std::size_t index = 1; index < kInlineCount; ++index) {
    ASSERT_TRUE(HandleAccess::Root<String>(scope, HandleAccess::Read(*first)));
  }
  const auto overflow = HandleAccess::Root<String>(scope, HandleAccess::Read(*first));
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().status, ExecutionStatus::OutOfMemory);
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), baseline);
  EXPECT_EQ(HandleAccess::Read(*first).AsString()->value(), "budget");
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
}

TEST(HandleScopeTest, RejectsForeignAndEmptyPersistentHandles) {
  runtime::Isolate first;
  runtime::Isolate second;
  HandleScope source_scope(first);
  HandleScope destination_scope(second);
  const auto source = HandleAccess::Root<Value>(source_scope, runtime::Value::Integer(42));
  ASSERT_TRUE(source);
  const auto persistent = source->Persist();
  ASSERT_TRUE(persistent);
  const auto rejected = persistent->ToLocal(destination_scope);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, ExecutionStatus::InvalidIsolate);
  const Persistent<Value> empty;
  const auto invalid = empty.ToLocal(source_scope);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().status, ExecutionStatus::InvalidEntry);
}

}  // namespace
}  // namespace vela
