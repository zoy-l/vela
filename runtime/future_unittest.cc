#include "runtime/future.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

constexpr std::int64_t kErrorValue = 13;
constexpr std::int64_t kExpectedValue = 42;
constexpr int kThreadCount = 8;
constexpr int kMessagesPerThread = 128;

void Drain(FutureScheduler& scheduler) {
  while (scheduler.PumpOne()) {
  }
}

struct ErrorNotificationCounts final {
  int unhandled = 0;
  int handled = 0;
  bool pumped = false;
};

ErrorNotificationCounts ObserveFailureLifecycle() {
  FutureScheduler scheduler;
  ErrorNotificationCounts counts;
  scheduler.SetUnhandledErrorHandler([&](const Future&, Value) { ++counts.unhandled; });
  scheduler.SetHandledErrorHandler([&](const Future&, Value) { ++counts.handled; });
  auto completer = scheduler.CreateCompleter();
  const auto future = completer.future();
  const auto completion = completer.CompleteError(Value::Integer(kErrorValue));
  if (!completion) {
    return counts;
  }
  counts.pumped = scheduler.PumpOne();
  future.Observe([](FutureState, Value) {});
  counts.pumped = scheduler.PumpOne() && counts.pumped;
  return counts;
}

int ObserveFailureWithHandler() {
  FutureScheduler scheduler;
  int unhandled = 0;
  scheduler.SetUnhandledErrorHandler([&](const Future&, Value) { ++unhandled; });
  auto completer = scheduler.CreateCompleter();
  completer.future().Observe([](FutureState, Value) {});
  const auto completion = completer.CompleteError(Value::Integer(kErrorValue));
  if (!completion) {
    return unhandled;
  }
  static_cast<void>(scheduler.PumpOne());
  return unhandled;
}

TEST(FutureTest, CompletionNotifiesObserversAsMicrotasksInRegistrationOrder) {
  FutureScheduler scheduler;
  auto completer = scheduler.CreateCompleter();
  std::vector<int> notifications;
  completer.future().Observe([&](FutureState, Value) { notifications.push_back(1); });
  completer.future().Observe([&](FutureState, Value) { notifications.push_back(2); });
  constexpr auto kValue = 7;
  const auto completion = completer.Complete(Value::Integer(kValue));
  ASSERT_TRUE(completion.has_value());
  EXPECT_TRUE(notifications.empty());
  ASSERT_TRUE(scheduler.PumpOne());
  EXPECT_EQ(notifications, (std::vector<int>{1, 2}));
}

TEST(FutureTest, ValueFactoryCreatesCompletedFuture) {
  const Future future = Future::Value(Value::Integer(9));
  ASSERT_EQ(future.state(), FutureState::Succeeded);
  const auto value = future.value();
  ASSERT_TRUE(value.has_value());
  const auto integer = value.value_or(Value::Null()).AsInteger();
  EXPECT_EQ(integer, 9);
}

TEST(FutureTest, ReportsUnhandledErrorAndLaterHandledError) {
  const auto counts = ObserveFailureLifecycle();
  EXPECT_TRUE(counts.pumped);
  EXPECT_EQ(counts.unhandled, 1);
  EXPECT_EQ(counts.handled, 1);
}

TEST(FutureTest, ObservedFailureDoesNotReportAsUnhandled) {
  EXPECT_EQ(ObserveFailureWithHandler(), 0);
}

TEST(FutureTest, TimerCancellationAndPeriodicRescheduling) {
  FutureScheduler scheduler;
  int calls = 0;
  auto cancelled = scheduler.After(std::chrono::milliseconds(0), [&] { ++calls; });
  cancelled.Cancel();
  EXPECT_FALSE(scheduler.PumpOne());

  static_cast<void>(scheduler.Periodic(std::chrono::milliseconds(1), [&] { ++calls; }));
  ASSERT_TRUE(scheduler.PumpOne());
  ASSERT_TRUE(scheduler.PumpOne());
  EXPECT_EQ(calls, 2);
}

TEST(FutureTest, CancellationTokenRunsCallbacksOnce) {
  CancellationToken token;
  int calls = 0;
  token.Observe([&] { ++calls; });
  token.Cancel();
  token.Cancel();
  EXPECT_TRUE(token.IsCancelled());
  EXPECT_EQ(calls, 1);
}

TEST(FutureTest, WaitAndRaceObserveAllCompletionPaths) {
  FutureScheduler scheduler;
  auto first = scheduler.CreateCompleter();
  auto second = scheduler.CreateCompleter();
  const std::array<Future, 2> inputs = {first.future(), second.future()};
  auto waited = Future::Wait(inputs, scheduler);
  auto raced = Future::Race(inputs, scheduler);
  constexpr auto kFirst = 1;
  constexpr auto kSecond = 2;
  const auto first_completion = first.Complete(Value::Integer(kFirst));
  ASSERT_TRUE(first_completion.has_value());
  ASSERT_TRUE(scheduler.PumpOne());
  EXPECT_EQ(raced.state(), FutureState::Succeeded);
  EXPECT_EQ(waited.state(), FutureState::Pending);
  const auto second_completion = second.Complete(Value::Integer(kSecond));
  ASSERT_TRUE(second_completion.has_value());
  ASSERT_TRUE(scheduler.PumpOne());
  EXPECT_EQ(waited.state(), FutureState::Succeeded);
}

TEST(FutureTest, TimeoutUsesReplacementFuture) {
  constexpr auto kTimeoutResult = 8;
  FutureScheduler scheduler;
  auto source = scheduler.CreateCompleter();
  const auto timeout = source.future().Timeout(std::chrono::milliseconds(0), scheduler, [](Value) {
    return Future::Value(Value::Integer(kTimeoutResult));
  });
  ASSERT_TRUE(scheduler.PumpOne());
  EXPECT_EQ(timeout.state(), FutureState::Succeeded);
  const auto timeout_value = timeout.value();
  ASSERT_TRUE(timeout_value.has_value());
  EXPECT_EQ(timeout_value.value_or(Value::Null()).AsInteger(), kTimeoutResult);
}

TEST(FutureTest, CrossThreadCompletionOnlyPublishesOnOwnerThread) {
  FutureScheduler scheduler;
  auto completer = scheduler.CreateHostCompleter();
  const auto owner = std::this_thread::get_id();
  std::thread::id notified_thread;
  bool notified = false;
  Value observed = Value::Null();
  completer.future().Observe([&](FutureState, Value value) {
    notified_thread = std::this_thread::get_id();
    observed = value;
    notified = true;
  });
  std::atomic_bool accepted{false};
  std::thread producer(
      [&] { accepted = completer.Complete(Value::Integer(kExpectedValue)).has_value(); });
  producer.join();
  ASSERT_TRUE(accepted.load());
  EXPECT_EQ(completer.future().state(), FutureState::Pending);
  EXPECT_FALSE(notified);
  Drain(scheduler);
  EXPECT_TRUE(notified);
  EXPECT_EQ(observed.AsInteger(), kExpectedValue);
  EXPECT_EQ(notified_thread, owner);
}

TEST(FutureTest, ConcurrentProducersKeepPerProducerFifo) {
  constexpr int kProducers = 8;
  constexpr int kMessages = 128;
  FutureScheduler scheduler;
  auto producer = scheduler.CreateHostCompleter();
  std::array<int, kProducers> next{};
  std::vector<std::thread> workers;
  workers.reserve(kProducers);
  int notifications = 0;
  bool in_order = true;
  workers.reserve(kProducers);
  for (int thread = 0; thread < kProducers; ++thread) {
    workers.emplace_back([&, thread] {
      for (int index = 0; index < kMessages; ++index) {
        scheduler.PostMicrotask([&, thread, index] {
          in_order = (next.at(static_cast<std::size_t>(thread))++ == index) && in_order;
          ++notifications;
        });
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  ASSERT_TRUE(producer.Complete(Value::Null()));
  Drain(scheduler);
  EXPECT_EQ(notifications, kProducers * kMessages);
  EXPECT_TRUE(in_order);
}

TEST(FutureTest, ConcurrentCompletionHasOneWinner) {
  FutureScheduler scheduler;
  auto completer = scheduler.CreateHostCompleter();
  std::atomic_int winners{0};
  std::vector<std::thread> producers;
  producers.reserve(kThreadCount);
  producers.reserve(kThreadCount);
  for (int index = 0; index < kThreadCount; ++index) {
    producers.emplace_back([&, index] {
      if (completer.Complete(Value::Integer(index))) {
        ++winners;
      }
    });
  }
  for (auto& producer : producers) {
    producer.join();
  }
  Drain(scheduler);
  EXPECT_EQ(winners.load(), 1);
  EXPECT_EQ(completer.future().state(), FutureState::Succeeded);
}

TEST(FutureTest, HostMessageInterruptsTimerWait) {
  FutureScheduler scheduler;
  bool timer_fired = false;
  bool host_fired = false;
  auto timer = scheduler.After(std::chrono::seconds(1), [&] { timer_fired = true; });
  std::thread producer([&] { scheduler.PostMicrotask([&] { host_fired = true; }); });
  ASSERT_TRUE(scheduler.PumpOne());
  producer.join();
  EXPECT_TRUE(host_fired);
  EXPECT_FALSE(timer_fired);
  timer.Cancel();
}

TEST(FutureTest, CancelledTimerReleasesCallbackImmediately) {
  FutureScheduler scheduler;
  auto retained = std::make_shared<int>(kExpectedValue);
  const std::weak_ptr<int> lifetime = retained;
  auto timer = scheduler.After(std::chrono::hours(1), [retained] {});
  retained.reset();
  EXPECT_FALSE(lifetime.expired());
  timer.Cancel();
  EXPECT_TRUE(lifetime.expired());
  EXPECT_FALSE(scheduler.PumpOne());
}

TEST(FutureTest, ConcurrentCancellationObserversRunExactlyOnce) {
  CancellationToken token;
  std::atomic_int called{0};
  std::vector<std::thread> observers;
  observers.reserve(kThreadCount);
  observers.reserve(kThreadCount);
  for (int index = 0; index < kThreadCount; ++index) {
    observers.emplace_back([&] {
      for (int count = 0; count < kMessagesPerThread; ++count) {
        token.Observe([&] { ++called; });
      }
    });
  }
  token.Cancel();
  for (auto& observer : observers) {
    observer.join();
  }
  token.Cancel();
  EXPECT_EQ(called.load(), kThreadCount * kMessagesPerThread);
}

TEST(FutureTest, HandlerAttachedDuringCheckpointSuppressesUnhandledReport) {
  FutureScheduler scheduler;
  int unhandled = 0;
  scheduler.SetUnhandledErrorHandler([&](const Future&, Value) { ++unhandled; });
  const auto future = Future::Error(Value::Integer(1), &scheduler);
  scheduler.PostMicrotask([future] { future.Observe([](FutureState, Value) {}); });
  ASSERT_TRUE(scheduler.PumpOne());
  EXPECT_EQ(unhandled, 0);
}

TEST(FutureTest, HeapOwnedResultCyclesDoNotBecomeExternalRoots) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto* layout = isolate.CreateClassLayout(1, std::array<std::uint32_t, 1>{0});
  {
    Future future(&isolate.scheduler());
    const auto object = Value::Object(heap.AllocateFuture(future));
    RootRange root(heap, &object, 1);
    auto* holder = heap.AllocateInstance(*layout);
    ASSERT_TRUE(heap.SetField(*holder, 0, object));
    ASSERT_TRUE(future.Complete(Value::Object(holder)));
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0);
}

TEST(FutureTest, BindingRejectsDifferentIsolate) {
  Isolate first;
  Isolate second;
  auto future = Future::Value(Value::Integer(kExpectedValue));
  ASSERT_TRUE(future.Bind(first.scheduler(), first.heap()));
  const auto rebound = future.Bind(second.scheduler(), second.heap());
  ASSERT_FALSE(rebound);
  EXPECT_EQ(rebound.error(), FutureBindingError::DifferentIsolate);
}

TEST(FutureTest, BindingSchedulesUnboundNativeCompletion) {
  Isolate first;
  auto future = Future::Value(Value::Integer(kExpectedValue));
  ASSERT_TRUE(future.Bind(first.scheduler(), first.heap()));
  bool notified = false;
  Value observed = Value::Null();
  future.Observe([&](FutureState, Value value) {
    observed = value;
    notified = true;
  });
  EXPECT_FALSE(notified);
  ASSERT_TRUE(first.scheduler().PumpOne());
  EXPECT_TRUE(notified);
  EXPECT_EQ(observed.AsInteger(), kExpectedValue);
}

}  // namespace
}  // namespace vela::runtime
