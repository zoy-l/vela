#include "runtime/future.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "runtime/isolate.h"
#include "runtime/thread_destruction_test_support.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

constexpr std::int64_t kCompletionValue = 42;

TEST(FutureCompletionEndpointTest, PublishesOnOwnerThread) {
  Isolate isolate(1);
  auto& scheduler = isolate.scheduler();
  auto completer = scheduler.CreateHostCompleter();
  auto future = completer.future();
  future.AttachHeap(isolate.heap());
  auto endpoint = std::move(completer).Detach();
  const auto owner = std::this_thread::get_id();
  std::thread::id notified_on;
  future.Observe([&](FutureState, Value) { notified_on = std::this_thread::get_id(); });
  std::atomic_bool accepted = false;
  std::thread host([endpoint, &accepted] {
    accepted = endpoint.Complete(Value::Integer(kCompletionValue)).has_value();
  });
  const auto completed = scheduler.PumpUntil(future, FuturePumpMode::WaitForHost);
  host.join();
  EXPECT_TRUE(accepted);
  EXPECT_TRUE(completed);
  const auto value = future.value();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value.value_or(Value::Integer(0)).AsInteger(), kCompletionValue);
  EXPECT_EQ(notified_on, owner);
}

TEST(FutureCompletionEndpointTest, CompletesOnlyOnce) {
  FutureScheduler scheduler;
  const auto endpoint = scheduler.CreateHostCompleter().Detach();
  ASSERT_TRUE(endpoint.Complete(Value::Integer(kCompletionValue)));
  const auto duplicate = endpoint.Complete(Value::Null());
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error(), FutureCompletionError::AlreadyCompleted);
}

TEST(FutureCompletionEndpointTest, LastHostHandleReleasesProducerAndPendingReferencesOnOwner) {
  FutureScheduler scheduler;
  std::thread::id released_on;
  auto completer = scheduler.CreateHostCompleter();
  completer.future().Observe(
      [resource = std::make_shared<test::DestructionThread>(released_on)](FutureState, Value) {});
  auto endpoint = std::move(completer).Detach();
  std::thread host([endpoint = std::move(endpoint)] {});
  host.join();
  EXPECT_EQ(released_on, std::thread::id{});
  ASSERT_TRUE(scheduler.PumpOne(FuturePumpMode::WaitForHost));
  EXPECT_EQ(released_on, std::this_thread::get_id());
  EXPECT_FALSE(scheduler.PumpOne(FuturePumpMode::WaitForHost));
}

TEST(FutureCompletionEndpointTest, EndpointOutlivesIsolateAndRejectsLateCompletion) {
  FutureCompletionEndpoint endpoint;
  std::weak_ptr<int> retained;
  {
    Isolate isolate(1);
    auto completer = isolate.scheduler().CreateHostCompleter();
    auto marker = std::make_shared<int>(0);
    retained = marker;
    completer.future().Observe([marker](FutureState, Value) { ++*marker; });
    endpoint = std::move(completer).Detach();
  }
  EXPECT_TRUE(retained.expired());
  std::atomic_bool closed = false;
  std::thread host([endpoint, &closed] {
    const auto completion = endpoint.Complete(Value::Integer(kCompletionValue));
    closed = !completion && completion.error() == FutureCompletionError::Closed;
  });
  host.join();
  EXPECT_TRUE(closed);
  const auto failure = endpoint.CompleteError(Value::Null());
  ASSERT_FALSE(failure);
  EXPECT_EQ(failure.error(), FutureCompletionError::Closed);
}

TEST(FutureCompletionEndpointTest, ShutdownDiscardsQueuedCompletionAndTimers) {
  Isolate isolate;
  auto& scheduler = isolate.scheduler();
  bool called = false;
  auto completer = scheduler.CreateHostCompleter();
  completer.future().Observe([&](FutureState, Value) { called = true; });
  auto endpoint = std::move(completer).Detach();
  static_cast<void>(scheduler.After(std::chrono::milliseconds(0), [&] { called = true; }));
  std::atomic_bool accepted = false;
  std::thread host([endpoint, &accepted] {
    accepted = endpoint.Complete(Value::Integer(kCompletionValue)).has_value();
  });
  host.join();
  ASSERT_TRUE(accepted);
  scheduler.Shutdown();
  EXPECT_FALSE(scheduler.PumpOne(FuturePumpMode::WaitForHost));
  EXPECT_FALSE(called);
}

TEST(FutureCompletionEndpointTest, ConcurrentShutdownAndCompletionHaveNoLateCallbacks) {
  constexpr std::uint32_t kRuns = 64;
  for (std::uint32_t run = 0; run < kRuns; ++run) {
    FutureCompletionEndpoint endpoint;
    auto isolate = std::make_unique<Isolate>(1);
    auto completer = isolate->scheduler().CreateHostCompleter();
    bool called = false;
    completer.future().Observe([&](FutureState, Value) { called = true; });
    endpoint = std::move(completer).Detach();
    std::atomic_bool start = false;
    std::atomic_bool valid = false;
    std::thread host([endpoint, &start, &valid] {
      start.wait(false);
      const auto completion = endpoint.Complete(Value::Integer(kCompletionValue));
      valid = completion.has_value() || completion.error() == FutureCompletionError::Closed;
    });
    start = true;
    start.notify_one();
    isolate.reset();
    host.join();
    EXPECT_TRUE(valid);
    EXPECT_FALSE(called);
  }
}

TEST(FutureCompletionEndpointTest, OldEndpointCannotCompleteNewRun) {
  FutureCompletionEndpoint old_endpoint;
  {
    Isolate isolate;
    old_endpoint = isolate.scheduler().CreateHostCompleter().Detach();
  }
  Isolate isolate;
  auto completer = isolate.scheduler().CreateHostCompleter();
  auto future = completer.future();
  const auto endpoint = std::move(completer).Detach();
  const auto stale = old_endpoint.Complete(Value::Integer(kCompletionValue));
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error(), FutureCompletionError::Closed);
  EXPECT_EQ(future.state(), FutureState::Pending);
  ASSERT_TRUE(endpoint.Complete(Value::Integer(kCompletionValue)));
  ASSERT_TRUE(isolate.scheduler().PumpUntil(future));
  const auto value = future.value();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value.value_or(Value::Integer(0)).AsInteger(), kCompletionValue);
}

}  // namespace
}  // namespace vela::runtime
