#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_class.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_host_type.h"
#include "runtime/value/host_object.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

struct TrackedHost final {
  explicit TrackedHost(std::uint32_t& destroyed) noexcept : destroyed(&destroyed) { ++constructed; }
  ~TrackedHost() { ++*destroyed; }
  TrackedHost(const TrackedHost&) = delete;
  TrackedHost& operator=(const TrackedHost&) = delete;
  TrackedHost(TrackedHost&&) = delete;
  TrackedHost& operator=(TrackedHost&&) = delete;
  std::uint32_t* destroyed;
  inline static std::uint32_t constructed = 0;
};

void DeleteHost(void* pointer) noexcept {
  std::default_delete<TrackedHost>{}(static_cast<TrackedHost*>(pointer));
}

}  // namespace

template <>
struct NativeClassTraits<TrackedHost> final {
  static constexpr std::string_view kKey = "test.Tracked";
};

namespace {

struct HostObjectTest : testing::Test {
  void SetUp() override {
    std::vector<NativeClass> module;
    module.push_back(NativeClass::Define<TrackedHost>({}));
    ASSERT_TRUE(isolate_.RegisterNativeModule(std::move(module)).has_value());
    layout_ = isolate_.FindNativeClass("test.Tracked")->layout;
  }

  std::uint32_t destroyed_ = 0;
  Isolate isolate_;
  ClassLayout* layout_ = nullptr;
};

TEST_F(HostObjectTest, UniqueOwnershipIsRootedAccountedAndReleasedOnce) {
  auto& heap = isolate_.heap();
  auto owned = std::make_unique<TrackedHost>(destroyed_);
  auto* native = owned.get();
  const auto wrapped = heap.WrapHostObject(*layout_, sizeof(TrackedHost),
                                           HostObjectStorage(owned.release(), &DeleteHost));
  ASSERT_TRUE(wrapped.has_value());
  auto value = Value::Object(*wrapped);
  const RootRange roots(heap, &value, 1);
  EXPECT_EQ(HostObjectPointer(**wrapped), native);
  EXPECT_EQ(Heap::KindOf(**wrapped), ObjectKind::Host);
  EXPECT_EQ(Heap::TypeOf(value).nominal, &layout_->nominal_type());
  EXPECT_GE(heap.allocated_bytes(), sizeof(TrackedHost) + sizeof(HeapObject));
  heap.Collect();
  EXPECT_EQ(destroyed_, 0U);
  EXPECT_EQ(heap.FindHostObject(layout_->nominal_type(), native), *wrapped);
  value = Value::Null();
  heap.Collect();
  EXPECT_EQ(destroyed_, 1U);
  EXPECT_EQ(heap.FindHostObject(layout_->nominal_type(), native), nullptr);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
}

TEST_F(HostObjectTest, SharedOwnershipPreservesCanonicalIdentityWithoutAnotherAllocation) {
  auto& heap = isolate_.heap();
  auto owner = std::make_shared<TrackedHost>(destroyed_);
  const auto first = heap.WrapHostObject(*layout_, sizeof(TrackedHost), HostObjectStorage(owner));
  ASSERT_TRUE(first.has_value());
  heap.ResetAllocationStatistics();
  const auto again = heap.WrapHostObject(*layout_, sizeof(TrackedHost), HostObjectStorage(owner));
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*again, *first);
  EXPECT_EQ(owner.use_count(), 2);
  EXPECT_EQ(heap.allocation_statistics().count, 0U);
  heap.Collect();
  EXPECT_EQ(destroyed_, 0U);
  EXPECT_EQ(owner.use_count(), 1);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
  owner.reset();
  EXPECT_EQ(destroyed_, 1U);
}

TEST_F(HostObjectTest, RejectsDuplicateUniqueOwnershipWithoutDeletingTheExistingObject) {
  auto& heap = isolate_.heap();
  auto owned = std::make_unique<TrackedHost>(destroyed_);
  auto* pointer = owned.release();
  const auto first =
      heap.WrapHostObject(*layout_, sizeof(TrackedHost), HostObjectStorage(pointer, &DeleteHost));
  ASSERT_TRUE(first.has_value());
  const auto duplicate =
      heap.WrapHostObject(*layout_, sizeof(TrackedHost), HostObjectStorage(pointer, &DeleteHost));
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error(), HostObjectError::OwnershipConflict);
  EXPECT_EQ(destroyed_, 0U);
  heap.Collect();
  EXPECT_EQ(destroyed_, 1U);
}

TEST_F(HostObjectTest, RejectsDifferentControlBlocksAndReleasesTheCandidateOwner) {
  auto& heap = isolate_.heap();
  auto owner = std::make_shared<TrackedHost>(destroyed_);
  std::shared_ptr<void> first(std::make_shared<std::shared_ptr<TrackedHost>>(owner), owner.get());
  const auto wrapped =
      heap.WrapHostObject(*layout_, sizeof(TrackedHost), HostObjectStorage(std::move(first)));
  ASSERT_TRUE(wrapped.has_value());
  std::shared_ptr<void> other(std::make_shared<std::shared_ptr<TrackedHost>>(owner), owner.get());
  const auto conflict =
      heap.WrapHostObject(*layout_, sizeof(TrackedHost), HostObjectStorage(std::move(other)));
  ASSERT_FALSE(conflict);
  EXPECT_EQ(conflict.error(), HostObjectError::OwnershipConflict);
  EXPECT_EQ(owner.use_count(), 2);
  heap.Collect();
  EXPECT_EQ(owner.use_count(), 1);
  owner.reset();
  EXPECT_EQ(destroyed_, 1U);
}

TEST_F(HostObjectTest, RejectsAFlaggedPointerWithoutSharedLifetime) {
  auto owner = std::make_unique<TrackedHost>(destroyed_);
  std::shared_ptr<void> empty;
  std::shared_ptr<void> invalid(empty, owner.get());
  const auto wrapped = isolate_.heap().WrapHostObject(*layout_, sizeof(TrackedHost),
                                                      HostObjectStorage(std::move(invalid)));
  ASSERT_FALSE(wrapped);
  EXPECT_EQ(wrapped.error(), HostObjectError::InvalidSharedOwner);
  EXPECT_EQ(isolate_.heap().allocated_bytes(), 0U);
  EXPECT_EQ(destroyed_, 0U);
}

TEST_F(HostObjectTest, EveryAllocationFailureReleasesOwnershipAndReservedMemory) {
  constexpr std::uint64_t kAllocationSteps = 3;
  auto& heap = isolate_.heap();
  for (std::uint64_t allowed = 0; allowed <= kAllocationSteps; ++allowed) {
    SCOPED_TRACE(allowed);
    heap.ResetAllocationStatistics();
    heap.SetAllocationCountLimit(allowed);
    auto owner = std::make_unique<TrackedHost>(destroyed_);
    const auto wrapped = heap.WrapHostObject(*layout_, sizeof(TrackedHost),
                                             HostObjectStorage(owner.release(), &DeleteHost));
    if (allowed < kAllocationSteps) {
      ASSERT_FALSE(wrapped);
      EXPECT_EQ(wrapped.error(), HostObjectError::OutOfMemory);
    } else {
      ASSERT_TRUE(wrapped.has_value());
    }
    heap.Collect();
    EXPECT_EQ(destroyed_, allowed + 1);
    EXPECT_EQ(heap.allocated_bytes(), 0U);
  }
}

TEST_F(HostObjectTest, ReusedNativeAddressCannotMatchACollectedWrapper) {
  auto& heap = isolate_.heap();
  const std::shared_ptr<void> storage(::operator new(sizeof(TrackedHost)),
                                      [](void* pointer) noexcept { ::operator delete(pointer); });
  auto* pointer = static_cast<TrackedHost*>(storage.get());
  const auto create = [&] {
    std::construct_at(pointer, destroyed_);
    return std::shared_ptr<void>(pointer, [storage](void* object) noexcept {
      std::destroy_at(static_cast<TrackedHost*>(object));
    });
  };
  const auto first =
      heap.WrapHostObject(*layout_, sizeof(TrackedHost), HostObjectStorage(create()));
  ASSERT_TRUE(first.has_value());
  heap.Collect();
  EXPECT_EQ(destroyed_, 1U);
  EXPECT_EQ(heap.FindHostObject(layout_->nominal_type(), pointer), nullptr);
  const auto second =
      heap.WrapHostObject(*layout_, sizeof(TrackedHost), HostObjectStorage(create()));
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(heap.FindHostObject(layout_->nominal_type(), pointer), *second);
  heap.Collect();
  EXPECT_EQ(destroyed_, 2U);
}

TEST_F(HostObjectTest, WeakIndexGrowthPreservesRootsAndDoesNotRetainCollectedObjects) {
  constexpr std::size_t kObjectCount = 32;
  std::array<Value, kObjectCount> values;
  auto& heap = isolate_.heap();
  const RootRange roots(heap, values.data(), values.size());
  for (auto& value : values) {
    auto owned = std::make_unique<TrackedHost>(destroyed_);
    const auto wrapped = heap.WrapHostObject(*layout_, sizeof(TrackedHost),
                                             HostObjectStorage(owned.release(), &DeleteHost));
    ASSERT_TRUE(wrapped.has_value());
    value = Value::Object(*wrapped);
    heap.WriteRootBarrier(std::span(&value, 1));
  }
  heap.Collect();
  EXPECT_EQ(destroyed_, 0U);
  values.fill(Value::Null());
  heap.Collect();
  EXPECT_EQ(destroyed_, kObjectCount);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
}

TEST_F(HostObjectTest, WeakIndexCompactionKeepsLiveIdentityWhenAllocationFails) {
  constexpr std::size_t kObjectCount = 64;
  std::array<Value, kObjectCount> values;
  auto& heap = isolate_.heap();
  const RootRange roots(heap, values.data(), values.size());
  const auto wrap = [&] {
    auto owned = std::make_unique<TrackedHost>(destroyed_);
    return heap.WrapHostObject(*layout_, sizeof(TrackedHost),
                               HostObjectStorage(owned.release(), &DeleteHost));
  };
  const auto first = wrap();
  ASSERT_TRUE(first);
  values.front() = Value::Object(*first);
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  auto* pointer = HostObjectPointer(**first);
  for (auto& value : std::span(values).subspan(1)) {
    const auto wrapped = wrap();
    ASSERT_TRUE(wrapped);
    value = Value::Object(*wrapped);
    heap.WriteRootBarrier(std::span(&value, 1));
  }
  heap.FinishCollection();
  std::ranges::fill(std::span(values).subspan(1), Value::Null());
  heap.SetAllocationCountLimit(0);
  heap.Collect();
  EXPECT_EQ(destroyed_, kObjectCount - 1);
  EXPECT_EQ(heap.FindHostObject(layout_->nominal_type(), pointer), *first);
  EXPECT_GT(heap.allocated_bytes(), baseline);
  heap.SetAllocationCountLimit(std::numeric_limits<std::uint64_t>::max());
  heap.Collect();
  EXPECT_EQ(heap.FindHostObject(layout_->nominal_type(), pointer), *first);
  EXPECT_EQ(heap.allocated_bytes(), baseline);
  values.front() = Value::Null();
  heap.Collect();
  EXPECT_EQ(destroyed_, kObjectCount);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
}

TEST_F(HostObjectTest, WrapperPublicationSurvivesCollectionOfTheLastIndexedObject) {
  auto& heap = isolate_.heap();
  const NativeContext context(isolate_);
  auto first = context.Construct<TrackedHost>(destroyed_);
  ASSERT_TRUE(first);
  ASSERT_TRUE(context.Own(std::move(*first)));
  auto next = context.Construct<TrackedHost>(destroyed_);
  ASSERT_TRUE(next);
  auto* pointer = next->value.get();

  heap.StartCollection();
  const auto wrapped = context.Own(std::move(*next));
  ASSERT_TRUE(wrapped);
  EXPECT_EQ(destroyed_, 1U);
  EXPECT_EQ(heap.FindHostObject(layout_->nominal_type(), pointer), wrapped->AsObject());
  heap.Collect();
  EXPECT_EQ(destroyed_, 2U);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
}

TEST_F(HostObjectTest, ConstructionReservesBudgetBeforeCallingCppAndReleasesFailedBundles) {
  auto& heap = isolate_.heap();
  const NativeContext context(isolate_);
  const auto before = TrackedHost::constructed;
  heap.SetAllocationLimit(0);
  const auto denied = context.Construct<TrackedHost>(destroyed_);
  ASSERT_FALSE(denied);
  EXPECT_EQ(denied.error().status, ExecutionStatus::OutOfMemory);
  EXPECT_EQ(TrackedHost::constructed, before);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  {
    auto bundle = context.Construct<TrackedHost>(destroyed_);
    ASSERT_TRUE(bundle);
    EXPECT_EQ(TrackedHost::constructed, before + 1);
    EXPECT_EQ(heap.allocated_bytes(), sizeof(TrackedHost));
  }
  EXPECT_EQ(destroyed_, 1U);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
}

TEST_F(HostObjectTest, ExternalMemoryUpdatesAreRootedAndPreserveTheOldBudgetOnFailure) {
  auto& heap = isolate_.heap();
  const NativeContext context(isolate_);
  auto bundle = context.Construct<TrackedHost>(destroyed_);
  ASSERT_TRUE(bundle);
  auto& native = *bundle->value;
  auto owned = context.Own(std::move(*bundle));
  ASSERT_TRUE(owned);
  auto value = *owned;
  const RootRange roots(heap, &value, 1);
  const auto baseline = heap.allocated_bytes();
  constexpr std::size_t kExtraBytes = 4096;
  ASSERT_TRUE(context.ReportExternalMemory(native, kExtraBytes));
  EXPECT_EQ(heap.allocated_bytes(), baseline + kExtraBytes);
  heap.SetAllocationLimit(heap.allocated_bytes());
  const auto denied = context.ReportExternalMemory(native, 2 * kExtraBytes);
  ASSERT_FALSE(denied);
  EXPECT_EQ(denied.error().status, ExecutionStatus::OutOfMemory);
  EXPECT_EQ(heap.allocated_bytes(), baseline + kExtraBytes);
  EXPECT_EQ(destroyed_, 0U);
  ASSERT_TRUE(context.ReportExternalMemory(native, 0));
  EXPECT_EQ(heap.allocated_bytes(), baseline);
  value = Value::Null();
  heap.Collect();
  EXPECT_EQ(destroyed_, 1U);
  EXPECT_EQ(heap.allocated_bytes(), 0U);
}

TEST_F(HostObjectTest, ConstructionBundlesCannotCrossIsolatesButSharedOwnersCan) {
  Isolate other;
  std::vector<NativeClass> module;
  module.push_back(NativeClass::Define<TrackedHost>({}));
  ASSERT_TRUE(other.RegisterNativeModule(std::move(module)));
  const NativeContext first(isolate_);
  const NativeContext second(other);
  auto bundle = first.Construct<TrackedHost>(destroyed_);
  ASSERT_TRUE(bundle);
  const auto foreign = second.Own(std::move(*bundle));
  ASSERT_FALSE(foreign);
  EXPECT_EQ(foreign.error().status, ExecutionStatus::TypeError);
  EXPECT_EQ(destroyed_, 1U);
  EXPECT_EQ(isolate_.heap().allocated_bytes(), 0U);
  EXPECT_EQ(other.heap().allocated_bytes(), 0U);
  auto shared = std::make_shared<TrackedHost>(destroyed_);
  ASSERT_TRUE(first.Own(shared));
  ASSERT_TRUE(second.Own(shared));
  EXPECT_EQ(shared.use_count(), 3);
  isolate_.heap().Collect();
  EXPECT_EQ(shared.use_count(), 2);
  other.heap().Collect();
  EXPECT_EQ(shared.use_count(), 1);
  shared.reset();
  EXPECT_EQ(destroyed_, 2U);
}

}  // namespace
}  // namespace vela::runtime
