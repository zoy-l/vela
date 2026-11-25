#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
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

TEST(HandleScopeTest, LocalizedRootsSurviveIndependentReleaseAndFurtherScopeStorage) {
  runtime::Isolate isolate(1);
  auto& heap = isolate.heap();
  Persistent<String> saved;
  {
    HandleScope registration(isolate);
    const auto value = HandleAccess::Root<String>(
        registration, runtime::Value::String(heap.AllocateString("shared localization")));
    ASSERT_TRUE(value);
    auto persistent = value->Persist();
    ASSERT_TRUE(persistent);
    saved = std::move(*persistent);
  }
  auto first = std::make_unique<HandleScope>(isolate);
  auto second = std::make_unique<HandleScope>(isolate);
  const auto first_local = saved.ToLocal(*first);
  const auto second_local = saved.ToLocal(*second);
  ASSERT_TRUE(first_local);
  ASSERT_TRUE(second_local);
  saved.Reset();
  first.reset();
  const auto other = HandleAccess::Root<String>(
      *second, runtime::Value::String(heap.AllocateString("another local")));
  ASSERT_TRUE(other);
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 2U);
  EXPECT_EQ(HandleAccess::Read(*second_local).AsString()->value(), "shared localization");
  EXPECT_EQ(HandleAccess::Read(*other).AsString()->value(), "another local");
  second.reset();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
  EXPECT_TRUE(isolate.TryStop());
}

TEST(HandleScopeTest, FirstLocalizationNeedsNoNewAllocationAndStillBlocksStopping) {
  runtime::Isolate isolate(1);
  auto& heap = isolate.heap();
  Persistent<String> saved;
  {
    HandleScope registration(isolate);
    const auto value = HandleAccess::Root<String>(
        registration, runtime::Value::String(heap.AllocateString("budgeted localization")));
    ASSERT_TRUE(value);
    auto persistent = value->Persist();
    ASSERT_TRUE(persistent);
    saved = std::move(*persistent);
  }
  const auto baseline = heap.allocated_bytes();
  heap.SetAllocationLimit(baseline);
  {
    HandleScope scope(isolate);
    const auto local = saved.ToLocal(scope);
    ASSERT_TRUE(local);
    auto next = saved.ToLocal(scope);
    constexpr std::size_t kMaximumCopies = 1024;
    std::size_t copies = 0;
    while (next && copies < kMaximumCopies) {
      ++copies;
      next = saved.ToLocal(scope);
    }
    ASSERT_FALSE(next);
    EXPECT_EQ(next.error().status, ExecutionStatus::OutOfMemory);
    saved.Reset();
    heap.Collect();
    EXPECT_EQ(HandleAccess::Read(*local).AsString()->value(), "budgeted localization");
    EXPECT_FALSE(isolate.TryStop());
  }
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
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
  // 固定预算必须最终拒绝增长，测试不依赖句柄栈的内联容量。
  constexpr std::size_t kMaximumAttempts = 1024;
  std::size_t attempts = 0;
  auto overflow = HandleAccess::Root<String>(scope, HandleAccess::Read(*first));
  while (overflow && attempts < kMaximumAttempts) {
    ++attempts;
    overflow = HandleAccess::Root<String>(scope, HandleAccess::Read(*first));
  }
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().status, ExecutionStatus::OutOfMemory);
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), baseline);
  EXPECT_EQ(HandleAccess::Read(*first).AsString()->value(), "budget");
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
}

TEST(HandleScopeTest, IndependentlyReleasedScopesDoNotRetainEachOthersValues) {
  runtime::Isolate isolate(1);
  auto& heap = isolate.heap();
  const auto baseline = heap.allocated_bytes();
  auto first = std::make_unique<HandleScope>(isolate);
  auto second = std::make_unique<HandleScope>(isolate);
  ASSERT_TRUE(
      HandleAccess::Root<String>(*first, runtime::Value::String(heap.AllocateString("first"))));
  const auto kept =
      HandleAccess::Root<String>(*second, runtime::Value::String(heap.AllocateString("second")));
  ASSERT_TRUE(kept);
  first.reset();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1U);
  EXPECT_EQ(HandleAccess::Read(*kept).AsString()->value(), "second");
  constexpr std::size_t kCopies = 100;
  for (std::size_t index = 0; index < kCopies; ++index) {
    ASSERT_TRUE(HandleAccess::Root<String>(*second, HandleAccess::Read(*kept)));
  }
  heap.Collect();
  EXPECT_EQ(HandleAccess::Read(*kept).AsString()->value(), "second");
  second.reset();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
  EXPECT_EQ(heap.allocated_bytes(), baseline);
  EXPECT_TRUE(isolate.TryStop());
}

TEST(HandleScopeTest, OuterScopeCanGrowPastInnerSegmentsAndKeepTransferredLocals) {
  runtime::Isolate isolate(1);
  auto& heap = isolate.heap();
  HandleScope outer(isolate);
  std::vector<Local<String>> kept;
  for (std::size_t round = 0; round < 3; ++round) {
    HandleScope inner(isolate);
    const auto source = HandleAccess::Root<String>(
        inner, runtime::Value::String(heap.AllocateString("transferred")));
    ASSERT_TRUE(source);
    constexpr std::size_t kCopiesPerRound = 40;
    for (std::size_t index = 0; index < kCopiesPerRound; ++index) {
      const auto copied = HandleAccess::Root<String>(outer, HandleAccess::Read(*source));
      ASSERT_TRUE(copied);
      kept.push_back(*copied);
    }
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 3U);
  for (const auto& local : kept) {
    EXPECT_EQ(HandleAccess::Read(local).AsString()->value(), "transferred");
  }
}

TEST(HandleScopeTest, EmptyScopesHaveOneStableResourceWithExactActivityCount) {
  runtime::Isolate isolate;
  std::uint64_t identity = 0;
  {
    HandleScope outer(isolate);
    auto blockers = isolate.ReloadBlockers();
    ASSERT_EQ(blockers.size(), 1U);
    identity = blockers.front().identity;
    EXPECT_EQ(blockers.front().references, 1U);
    {
      HandleScope inner(isolate);
      blockers = isolate.ReloadBlockers();
      ASSERT_EQ(blockers.size(), 1U);
      EXPECT_EQ(blockers.front().identity, identity);
      EXPECT_EQ(blockers.front().references, 2U);
      EXPECT_FALSE(isolate.TryStop());
    }
    blockers = isolate.ReloadBlockers();
    ASSERT_EQ(blockers.size(), 1U);
    EXPECT_EQ(blockers.front().identity, identity);
    EXPECT_EQ(blockers.front().references, 1U);
  }
  EXPECT_TRUE(isolate.ReloadBlockers().empty());
  EXPECT_TRUE(isolate.TryStop());
}

TEST(HandleScopeTest, PublishingDuringIncrementalMarkRetainsTheCompleteObjectGraph) {
  runtime::Isolate isolate(1, runtime::GcScheduling::kHostDriven);
  auto& heap = isolate.heap();
  const std::array reference_fields{std::uint32_t{0}};
  const auto* layout = isolate.CreateClassLayout(1, reference_fields);
  {
    HandleScope scope(isolate);
    auto* text = heap.AllocateString("child");
    ASSERT_NE(text, nullptr);
    const auto value = runtime::Value::String(text);
    const runtime::RootRange temporary(heap, &value, 1);
    auto* object = heap.AllocateInstance(*layout, std::span(&value, 1));
    ASSERT_NE(object, nullptr);
    heap.FinishCollection();
    heap.StartCollection();
    const auto rooted = HandleAccess::Root<Value>(scope, runtime::Value::Object(object));
    ASSERT_TRUE(rooted);
    heap.FinishCollection();
    EXPECT_EQ(heap.object_count(), 2U);
    EXPECT_EQ(heap.LoadField(*HandleAccess::Read(*rooted).AsObject(), 0).AsString()->value(),
              "child");
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0U);
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
