#include "runtime/isolate.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "runtime/external_resources.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/native_api.h"
#include "runtime/native_function.h"
#include "runtime/rooted_value.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

std::int64_t HostAnswer() noexcept {
  constexpr std::int64_t kAnswer = 42;
  return kAnswer;
}

TEST(IsolateLifecycleTest, BindingSnapshotIsFixedAndSharesTheSameNativeImplementation) {
  Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<HostAnswer>("host.answer")));
  const auto snapshot = isolate.FreezeBindings();
  const auto refused = isolate.RegisterNative(NativeFunction::Bind<HostAnswer>("host.other"));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), NativeRegistrationError::BindingsFrozen);
  Isolate candidate(snapshot);
  EXPECT_EQ(isolate.FindNative("host.answer"), candidate.FindNative("host.answer"));
  const auto changed = candidate.RegisterNative(NativeFunction::Bind<HostAnswer>("host.other"));
  ASSERT_FALSE(changed);
  EXPECT_EQ(changed.error(), NativeRegistrationError::BindingsFrozen);
}

TEST(IsolateLifecycleTest, SharedRootBlocksStopUntilTheLastHostHandleIsReleased) {
  Isolate isolate;
  std::optional<RootedValue> first(std::in_place, isolate, Value::Null());
  auto second = first;
  const auto initial = isolate.ReloadBlockers();
  ASSERT_EQ(initial.size(), 1U);
  EXPECT_EQ(initial.front().kind, ExternalResourceKind::RootedValue);
  first.reset();
  const auto refused = isolate.TryStop();
  ASSERT_FALSE(refused);
  ASSERT_EQ(refused.error().size(), 1U);
  EXPECT_EQ(refused.error().front().identity, initial.front().identity);
  second.reset();
  EXPECT_TRUE(isolate.TryStop());
}

TEST(IsolateLifecycleTest, RawHostRootsAlsoRequireReleaseEvenWithoutManagedValues) {
  Isolate isolate;
  {
    const auto value = Value::Integer(42);
    const RootRange root(isolate.heap(), &value, 1);
    const auto refused = isolate.TryStop();
    ASSERT_FALSE(refused);
    ASSERT_EQ(refused.error().size(), 1U);
    EXPECT_EQ(refused.error().front().kind, ExternalResourceKind::RootRange);
  }
  EXPECT_TRUE(isolate.TryStop());
}

TEST(IsolateLifecycleTest, ExternalFutureCopiesHaveOneIdentityAndAnExactReferenceCount) {
  Isolate isolate;
  {
    Future future(&isolate.scheduler());
    std::optional<Future> copy(future);
    const auto resources = isolate.ReloadBlockers();
    ASSERT_EQ(resources.size(), 1U);
    EXPECT_EQ(resources.front().kind, ExternalResourceKind::Future);
    EXPECT_EQ(resources.front().references, 2U);
    copy.reset();
    const auto remaining = isolate.ReloadBlockers();
    ASSERT_EQ(remaining.size(), 1U);
    EXPECT_EQ(remaining.front().identity, resources.front().identity);
    EXPECT_EQ(remaining.front().references, 1U);
    EXPECT_FALSE(isolate.TryStop());
  }
  EXPECT_TRUE(isolate.TryStop());
}

TEST(IsolateLifecycleTest, UnboundFutureIsRegisteredWhenItBindsToTheIsolate) {
  Isolate isolate;
  {
    Future future;
    ASSERT_TRUE(future.Bind(isolate.scheduler(), isolate.heap()));
    const auto resources = isolate.ReloadBlockers();
    ASSERT_EQ(resources.size(), 1U);
    EXPECT_EQ(resources.front().kind, ExternalResourceKind::Future);
    EXPECT_FALSE(isolate.TryStop());
  }
  EXPECT_TRUE(isolate.TryStop());
}

TEST(IsolateLifecycleTest, PendingEndpointClosesAndReleasesReferencesWithoutBlockingStop) {
  Isolate isolate;
  auto marker = std::make_shared<int>(0);
  const std::weak_ptr<int> lifetime = marker;
  auto completer = isolate.scheduler().CreateHostCompleter();
  completer.future().Observe([marker](FutureState, Value) { ++*marker; });
  auto endpoint = std::move(completer).Detach();
  marker.reset();
  ASSERT_TRUE(isolate.ReloadBlockers().empty());
  ASSERT_TRUE(isolate.TryStop());
  EXPECT_TRUE(lifetime.expired());
  const auto completion = endpoint.Complete(Value::Null());
  ASSERT_FALSE(completion);
  EXPECT_EQ(completion.error(), FutureCompletionError::Closed);
}

TEST(IsolateLifecycleTest, RefusedStopPreservesPendingEventsAndOpenEndpoints) {
  Isolate isolate;
  auto completer = isolate.scheduler().CreateHostCompleter();
  bool called = false;
  completer.future().Observe([&](FutureState, Value) { called = true; });
  const auto endpoint = std::move(completer).Detach();
  {
    const RootedValue host_root(isolate, Value::Null());
    ASSERT_FALSE(isolate.TryStop());
    ASSERT_TRUE(endpoint.Complete(Value::Null()));
    ASSERT_TRUE(isolate.scheduler().PumpOne());
    EXPECT_TRUE(called);
  }
  EXPECT_TRUE(isolate.TryStop());
}

TEST(IsolateLifecycleTest, StopDisposesDeferredHostDestructionOnTheOwnerThread) {
  Isolate isolate;
  std::thread::id destroyed_on;
  struct Resource final {
    explicit Resource(std::thread::id& thread) : destroyed_on(&thread) {}
    ~Resource() { *destroyed_on = std::this_thread::get_id(); }
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;
    Resource(Resource&&) = delete;
    Resource& operator=(Resource&&) = delete;
    std::thread::id* destroyed_on;
  };
  Future future(&isolate.scheduler());
  future.Observe([resource = std::make_shared<Resource>(destroyed_on)](FutureState, Value) {});
  std::thread host([future = std::move(future)] {});
  host.join();
  EXPECT_EQ(destroyed_on, std::thread::id{});
  ASSERT_TRUE(isolate.TryStop());
  EXPECT_EQ(destroyed_on, std::this_thread::get_id());
}

TEST(IsolateLifecycleTest, OpaqueNativeHandlesAppearInStopDiagnostics) {
  Isolate isolate;
  VelaAotHandle* handle = nullptr;
  ASSERT_EQ(vela_aot_v1_string_create(&isolate, "text", 4, &handle), VELA_AOT_RETURNED);
  const auto refused = isolate.TryStop();
  ASSERT_FALSE(refused);
  ASSERT_EQ(refused.error().size(), 1U);
  EXPECT_EQ(refused.error().front().kind, ExternalResourceKind::NativeHandle);
  vela_aot_v1_handle_release(handle);
  EXPECT_TRUE(isolate.TryStop());
}

TEST(IsolateLifecycleTest, TimerHandleCanBeCancelledAfterItsIsolateHasStoppedAndDied) {
  Timer timer;
  auto marker = std::make_shared<int>(0);
  const std::weak_ptr<int> lifetime = marker;
  {
    Isolate isolate;
    timer = isolate.scheduler().Periodic(std::chrono::hours(1), [marker] { ++*marker; });
    marker.reset();
    ASSERT_TRUE(isolate.TryStop());
    EXPECT_TRUE(lifetime.expired());
  }
  std::thread host([timer]() mutable { timer.Cancel(); });
  host.join();
}

TEST(IsolateLifecycleTest, StopDuringSchedulerCallbackWaitsForTheOuterPumpToReturn) {
  Isolate isolate;
  bool refused = false;
  isolate.scheduler().PostMicrotask([&] { refused = !isolate.TryStop(); });
  ASSERT_TRUE(isolate.scheduler().PumpOne());
  EXPECT_TRUE(refused);
  EXPECT_TRUE(isolate.TryStop());
}

TEST(IsolateLifecycleTest, ConcurrentFinalHostReleaseNeverOutlivesSuccessfulStop) {
  constexpr std::uint32_t kRuns = 64;
  const auto owner = std::this_thread::get_id();
  struct Resource final {
    explicit Resource(std::thread::id& thread) : destroyed_on(&thread) {}
    ~Resource() { *destroyed_on = std::this_thread::get_id(); }
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;
    Resource(Resource&&) = delete;
    Resource& operator=(Resource&&) = delete;
    std::thread::id* destroyed_on;
  };
  for (std::uint32_t run = 0; run < kRuns; ++run) {
    auto isolate = std::make_unique<Isolate>();
    std::thread::id destroyed_on;
    Future future(&isolate->scheduler());
    future.Observe([resource = std::make_shared<Resource>(destroyed_on)](FutureState, Value) {});
    std::thread host([future = std::move(future)] {});
    if (isolate->TryStop()) {
      isolate.reset();
    }
    host.join();
    if (isolate != nullptr) {
      ASSERT_TRUE(isolate->TryStop());
      isolate.reset();
    }
    EXPECT_EQ(destroyed_on, owner);
  }
}

TEST(IsolateLifecycleTest, TimerCancellationRacingStopReleasesCallbacksOnTheOwner) {
  constexpr std::uint32_t kRuns = 64;
  for (std::uint32_t run = 0; run < kRuns; ++run) {
    Isolate isolate;
    auto marker = std::make_shared<int>(0);
    const std::weak_ptr<int> lifetime = marker;
    auto timer = isolate.scheduler().Periodic(std::chrono::hours(1), [marker] { ++*marker; });
    marker.reset();
    std::thread host([timer]() mutable { timer.Cancel(); });
    const auto stopped = isolate.TryStop();
    host.join();
    ASSERT_TRUE(stopped);
    EXPECT_TRUE(lifetime.expired());
  }
}

}  // namespace
}  // namespace vela::runtime
