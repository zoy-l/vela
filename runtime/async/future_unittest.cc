#include "runtime/async/future.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/execution_status.h"
#include "gtest/gtest.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/strings/string.h"
#include "runtime/testing/thread_destruction_test_support.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

constexpr std::int64_t kErrorValue = 13;
constexpr std::int64_t kExpectedValue = 42;
constexpr int kThreadCount = 8;
constexpr int kMessagesPerThread = 128;

void Drain(FutureScheduler& scheduler) {
  while (scheduler.PumpOne() == FuturePumpStep::Executed) {
  }
}

int ObserveFailureWithHandler() {
  FutureScheduler scheduler;
  int unhandled = 0;
  scheduler.SetUnhandledErrorHandler([&](const Future&, Value) { ++unhandled; });
  auto completer = scheduler.CreateHostCompleter();
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
  auto completer = scheduler.CreateHostCompleter();
  std::vector<int> notifications;
  completer.future().Observe([&](FutureState, Value) { notifications.push_back(1); });
  completer.future().Observe([&](FutureState, Value) { notifications.push_back(2); });
  constexpr auto kValue = 7;
  const auto completion = completer.Complete(Value::Integer(kValue));
  ASSERT_TRUE(completion.has_value());
  EXPECT_TRUE(notifications.empty());
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
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

TEST(FutureTest, ValueFactoryReportsForeignAdoptionAsInvalidIsolate) {
  Isolate isolate;
  Isolate other;
  Future source(&other.scheduler());
  const auto value = Value::Object(other.heap().AllocateFuture(source));
  ASSERT_NE(value.AsObject(), nullptr);
  const RootRange root(other.heap(), &value, 1);
  const auto future = Future::Value(value, &isolate.scheduler());
  EXPECT_EQ(future.state(), FutureState::Failed);
  EXPECT_EQ(future.result().diagnostic.status, ExecutionStatus::InvalidIsolate);
  EXPECT_TRUE(future.result().value.IsNull());
}

TEST(FutureTest, ReportsUnhandledErrorOnlyOnceAfterLateObservation) {
  FutureScheduler scheduler;
  int unhandled = 0;
  scheduler.SetUnhandledErrorHandler([&](const Future&, Value) { ++unhandled; });
  auto completer = scheduler.CreateHostCompleter();
  const auto future = completer.future();
  ASSERT_TRUE(completer.CompleteError(Value::Integer(kErrorValue)));
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  EXPECT_EQ(unhandled, 1);
  future.Observe([](FutureState, Value) {});
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  EXPECT_EQ(unhandled, 1);
}

TEST(FutureTest, ObservedFailureDoesNotReportAsUnhandled) {
  EXPECT_EQ(ObserveFailureWithHandler(), 0);
}

TEST(FutureTest, DirectResultObservationDoesNotQueueAnEmptyListener) {
  FutureScheduler scheduler;
  int unhandled = 0;
  scheduler.SetUnhandledErrorHandler([&](const Future&, Value) { ++unhandled; });
  Future future(&scheduler);
  future.MarkObserved();
  ASSERT_TRUE(future.CompleteError(Value::Integer(kErrorValue)));
  EXPECT_EQ(scheduler.PumpOne(), FuturePumpStep::Idle);
  EXPECT_EQ(unhandled, 0);
  EXPECT_EQ(future.result().value.AsInteger(), kErrorValue);
}

TEST(FutureTest, TimerCancellationAndPeriodicRescheduling) {
  FutureScheduler scheduler;
  int calls = 0;
  auto cancelled = scheduler.After(std::chrono::milliseconds(0), [&] { ++calls; });
  cancelled.Cancel();
  EXPECT_EQ(scheduler.PumpOne(), FuturePumpStep::Idle);

  static_cast<void>(scheduler.Periodic(std::chrono::milliseconds(1), [&] { ++calls; }));
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  EXPECT_EQ(calls, 2);
}

TEST(FutureTest, TimerCancellationRemovesRootAndMiddleEventsAndAllowsSelfCancellation) {
  constexpr int kPeriodic = 5;
  FutureScheduler scheduler;
  std::vector<int> calls;
  auto first = scheduler.After(std::chrono::milliseconds(0), [&] { calls.push_back(1); });
  auto second = scheduler.After(std::chrono::milliseconds(0), [&] { calls.push_back(2); });
  static_cast<void>(scheduler.After(std::chrono::milliseconds(0), [&] { calls.push_back(3); }));
  static_cast<void>(scheduler.After(std::chrono::milliseconds(0), [&] { calls.push_back(4); }));
  second.Cancel();
  first.Cancel();
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  EXPECT_EQ(scheduler.PumpOne(), FuturePumpStep::Idle);
  EXPECT_EQ(calls, (std::vector<int>{3, 4}));
  Timer periodic;
  periodic = scheduler.Periodic(std::chrono::milliseconds(1), [&] {
    calls.push_back(kPeriodic);
    periodic.Cancel();
  });
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  EXPECT_EQ(scheduler.PumpOne(), FuturePumpStep::Idle);
  EXPECT_EQ(calls, (std::vector<int>{3, 4, kPeriodic}));
}

TEST(FutureTest, TimerCancellationPreservesRemainingInsertionOrderAcrossHeapRepairs) {
  constexpr std::size_t kTimers = 128;
  constexpr std::size_t kPermutationStride = 73;
  FutureScheduler scheduler;
  std::array<Timer, kTimers> timers;
  std::array<bool, kTimers> cancelled{};
  std::vector<std::size_t> calls;
  for (std::size_t index = 0; index < kTimers; ++index) {
    timers[index] =
        scheduler.After(std::chrono::milliseconds(0), [&, index] { calls.push_back(index); });
  }
  for (std::size_t index = 0; index < kTimers / 2; ++index) {
    const auto selected = (index * kPermutationStride) % kTimers;
    timers[selected].Cancel();
    cancelled[selected] = true;
  }
  Drain(scheduler);
  std::vector<std::size_t> expected;
  for (std::size_t index = 0; index < kTimers; ++index) {
    if (!cancelled[index]) {
      expected.push_back(index);
    }
  }
  EXPECT_EQ(calls, expected);
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
  for (int thread = 0; thread < kProducers; ++thread) {
    workers.emplace_back([&, thread] {
      for (int index = 0; index < kMessages; ++index) {
        scheduler.PostMicrotask([&, thread, index] {
          in_order = (next[static_cast<std::size_t>(thread)]++ == index) && in_order;
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
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
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
  EXPECT_EQ(scheduler.PumpOne(), FuturePumpStep::Idle);
}

TEST(FutureTest, ConcurrentCancellationObserversRunExactlyOnce) {
  CancellationToken token;
  std::atomic_int called{0};
  std::vector<std::thread> observers;
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
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
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
  EXPECT_EQ(rebound.error(), ExecutionStatus::InvalidIsolate);
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
  ASSERT_EQ(first.scheduler().PumpOne(), FuturePumpStep::Executed);
  EXPECT_TRUE(notified);
  EXPECT_EQ(observed.AsInteger(), kExpectedValue);
}

TEST(FutureTest, ObserversBorrowOneCompletion) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  Future future(&isolate.scheduler());
  future.AttachHeap(heap);
  const FutureResult* first = nullptr;
  const FutureResult* second = nullptr;
  const FutureResult* late = nullptr;
  future.ObserveResult([&](const FutureResult& result) { first = &result; });
  future.ObserveResult([&](const FutureResult& result) { second = &result; });
  ASSERT_TRUE(future.CompleteError(
      Value::Integer(kErrorValue),
      {.message = "failure",
       .stack_trace = {{.function = "host", .source_path = "input.vela", .identity = {}}},
       .origin = {}}));
  ASSERT_EQ(isolate.scheduler().PumpOne(), FuturePumpStep::Executed);
  future.ObserveResult([&](const FutureResult& result) { late = &result; });
  ASSERT_EQ(isolate.scheduler().PumpOne(), FuturePumpStep::Executed);
  EXPECT_EQ(first, second);
  EXPECT_EQ(first, late);
}

TEST(FutureTest, MixedObserversPreserveOrderAfterPendingCompactionAndQueuedCancellation) {
  Isolate isolate(1);
  Future future(&isolate.scheduler());
  future.AttachHeap(isolate.heap());
  std::vector<int> calls;
  future.ObserveResult([&](const FutureResult&) { calls.push_back(1); });
  constexpr std::size_t kObservers = 8;
  constexpr int kLastObserver = 10;
  constexpr int kSurvivingObserver = 5;
  constexpr std::size_t kQueuedCancellation = 5;
  std::array<FutureObservation, kObservers> observations;
  for (std::size_t index = 0; index < observations.size(); ++index) {
    auto observation = future.SubscribeResult(
        [&, index](const FutureResult&) { calls.push_back(static_cast<int>(index) + 2); });
    ASSERT_TRUE(observation.has_value());
    observations[index] = std::move(*observation);
  }
  for (const auto index : {0U, 2U, 4U, 6U, 7U, 1U}) {
    observations[index].Cancel();
  }
  future.ObserveResult([&](const FutureResult&) { calls.push_back(kLastObserver); });
  ASSERT_TRUE(future.Complete(Value::Integer(kExpectedValue)));
  observations[kQueuedCancellation].Cancel();
  isolate.heap().Collect();
  Drain(isolate.scheduler());
  EXPECT_EQ(calls, (std::vector<int>{1, kSurvivingObserver, kLastObserver}));
}

TEST(FutureTest, PostedCompletionAndMultipleAdoptionsPreserveErrorDiagnostic) {
  constexpr std::size_t kMessageSize = 128;
  FutureScheduler scheduler;
  Future source(&scheduler);
  Future first(&scheduler);
  Future second(&scheduler);
  first.MarkObserved();
  second.MarkObserved();
  const FutureDiagnostic diagnostic{
      .message = std::string(kMessageSize, 'e'),
      .stack_trace = {{.function = "source", .source_path = "input.vela", .identity = {}}},
      .origin = {},
  };
  std::optional<FutureCompletionError> completion_error;
  std::thread host([copy = source, diagnostic, &completion_error]() mutable {
    if (const auto completion = copy.CompleteError(Value::Integer(kErrorValue), diagnostic);
        !completion) {
      completion_error = completion.error();
    }
  });
  host.join();
  ASSERT_FALSE(completion_error.has_value());
  ASSERT_TRUE(first.Adopt(source));
  ASSERT_TRUE(second.Adopt(source));
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  const auto error_identity = source.result().diagnostic.origin.identity;
  const std::array observed{&source, &first, &second};
  for (const auto* future : observed) {
    const auto& result = future->result();
    EXPECT_EQ(result.state, FutureState::Failed);
    EXPECT_EQ(result.value.AsInteger(), kErrorValue);
    EXPECT_EQ(result.diagnostic.message, diagnostic.message);
    ASSERT_EQ(result.diagnostic.stack_trace.size(), 1U);
    EXPECT_EQ(result.diagnostic.stack_trace.front().function, "source");
    EXPECT_EQ(result.diagnostic.origin.identity, error_identity);
  }
}

TEST(FutureTest, ObserverReferencesSurviveQueuedAndActiveCollection) {
  Isolate isolate(1);
  auto& heap = isolate.heap();
  Future future(&isolate.scheduler());
  future.AttachHeap(heap);
  auto references = std::make_shared<std::vector<Value>>();
  references->push_back(Value::String(heap.AllocateString("listener")));
  std::string observed;
  future.ObserveResult(
      [&](const FutureResult&) {
        heap.Collect();
        observed = references->front().AsString()->value();
      },
      references);
  ASSERT_TRUE(future.Complete(Value::Null()));
  heap.Collect();
  ASSERT_EQ(isolate.scheduler().PumpOne(), FuturePumpStep::Executed);
  EXPECT_EQ(observed, "listener");
  references.reset();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 0);
}

TEST(FutureTest, DiscardingQueuedObserversReleasesRootsAndSelfCaptures) {
  Isolate isolate;
  auto& heap = isolate.heap();
  Future future(&isolate.scheduler());
  future.AttachHeap(heap);
  auto references = std::make_shared<std::vector<Value>>();
  references->push_back(Value::String(heap.AllocateString("queued")));
  auto marker = std::make_shared<int>(kExpectedValue);
  const std::weak_ptr<int> lifetime = marker;
  future.ObserveResult([self = future.Internal(), marker](const FutureResult&) {}, references);
  references.reset();
  marker.reset();
  ASSERT_TRUE(future.Complete(Value::Null()));
  heap.Collect();
  EXPECT_EQ(heap.object_count(), 1);
  isolate.scheduler().Shutdown();
  heap.Collect();
  EXPECT_TRUE(lifetime.expired());
  EXPECT_EQ(heap.object_count(), 0);
}

TEST(FutureTest, DroppingNonfinalHostHandleDoesNotPostAnEmptyEvent) {
  FutureScheduler scheduler;
  Future future(&scheduler);
  std::thread host([copy = future] {});
  host.join();
  EXPECT_EQ(scheduler.PumpOne(), FuturePumpStep::Idle);
}

TEST(FutureTest, HostProducerAllowsIdlePumpAndBlockingPumpToHaveDifferentBehavior) {
  FutureScheduler scheduler;
  auto producer = scheduler.CreateHostCompleter();
  auto root = producer.future();
  EXPECT_EQ(scheduler.PumpUntil(root), FuturePumpResult::Idle);
  std::atomic_bool accepted = false;
  std::thread host(
      [&] { accepted = producer.Complete(Value::Integer(kExpectedValue)).has_value(); });
  const auto completed = scheduler.PumpUntil(root, FuturePumpMode::WaitForHost);
  host.join();
  EXPECT_TRUE(accepted.load());
  EXPECT_EQ(completed, FuturePumpResult::Completed);
  const auto result = root.value();
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result.value_or(Value::Null()).IsInteger());
  EXPECT_EQ(result.value_or(Value::Null()).AsInteger(), kExpectedValue);
}

TEST(FutureTest, FinalHostHandleDestroysPendingCallbacksOnOwnerThread) {
  FutureScheduler scheduler;
  std::thread::id destroyed_on;
  auto resource = std::make_shared<test::DestructionThread>(destroyed_on);
  Future future(&scheduler);
  future.ObserveResult([resource](const FutureResult&) {});
  resource.reset();
  std::thread host([owned = std::move(future)] {});
  host.join();
  EXPECT_EQ(destroyed_on, std::thread::id{});
  ASSERT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  EXPECT_EQ(destroyed_on, std::this_thread::get_id());
}

TEST(FutureTest, PostedSelfAdoptionFailsOnOwnerThreadWithTheSameReason) {
  Isolate isolate;
  auto& heap = isolate.heap();
  Future future(&isolate.scheduler());
  const auto value = Value::Object(heap.AllocateFuture(future));
  RootRange root(heap, &value, 1);
  const auto immediate = future.Complete(value);
  ASSERT_FALSE(immediate);
  EXPECT_EQ(immediate.error(), FutureCompletionError::SelfAdoption);
  std::atomic_bool accepted = false;
  std::thread host([&] { accepted = future.Complete(value).has_value(); });
  host.join();
  ASSERT_TRUE(accepted.load());
  ASSERT_EQ(isolate.scheduler().PumpOne(), FuturePumpStep::Executed);
  const auto diagnostic = future.error_diagnostic();
  ASSERT_TRUE(diagnostic.has_value());
  EXPECT_EQ(diagnostic.value_or(FutureDiagnostic{}).status, ExecutionStatus::FutureSelfAdoption);
}

TEST(FutureTest, PostedForeignAdoptionReportsInvalidIsolate) {
  Isolate isolate;
  Isolate other;
  Future future(&isolate.scheduler());
  future.AttachHeap(isolate.heap());
  const auto source = Value::Object(other.heap().AllocateFuture(Future(&other.scheduler())));
  RootRange root(other.heap(), &source, 1);
  const auto immediate = future.Complete(source);
  ASSERT_FALSE(immediate);
  EXPECT_EQ(immediate.error(), FutureCompletionError::DifferentIsolate);
  std::atomic_bool accepted = false;
  std::thread host([&] { accepted = future.Complete(source).has_value(); });
  host.join();
  ASSERT_TRUE(accepted.load());
  ASSERT_EQ(isolate.scheduler().PumpOne(), FuturePumpStep::Executed);
  const auto diagnostic = future.error_diagnostic();
  ASSERT_TRUE(diagnostic.has_value());
  EXPECT_EQ(diagnostic.value_or(FutureDiagnostic{}).status, ExecutionStatus::InvalidIsolate);
}

TEST(FutureTest, UnboundReentrantObserversKeepSynchronousNotificationOrder) {
  Future future;
  std::vector<int> notifications;
  future.ObserveResult([&](const FutureResult&) {
    notifications.push_back(1);
    future.ObserveResult([&](const FutureResult&) { notifications.push_back(3); });
  });
  future.ObserveResult([&](const FutureResult&) { notifications.push_back(2); });
  ASSERT_TRUE(future.Complete(Value::Null()));
  EXPECT_EQ(notifications, (std::vector<int>{1, 3, 2}));
}

}  // namespace
}  // namespace vela::runtime
