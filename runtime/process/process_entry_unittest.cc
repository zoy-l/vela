#include "runtime/process/process_entry.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ios>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_control.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

class ProcessEntryTest : public testing::Test {
 public:
  ProcessEntryTest() { isolate_.SetOutput(output_); }

  Value Wrap(Future future,
             std::optional<TypeConstraint> payload = TypeConstraint{.kind = ValueKind::Integer}) {
    auto* object = isolate_.heap().AllocateFuture(std::move(future));
    const auto* signature = isolate_.CreateTypeSignature({.parameters = {}, .result = payload});
    EXPECT_TRUE(Heap::BindRuntimeType(*object, {
                                                   .kind = ValueKind::Object,
                                                   .object_kind = ObjectKind::Future,
                                                   .signature = signature,
                                               }));
    return Value::Object(object);
  }

  Isolate isolate_;
  std::ostringstream output_;
  std::ostringstream errors_;
  const TypeConstraint integer_{.kind = ValueKind::Integer};
};

TEST_F(ProcessEntryTest, ScalarResultsAreOutputAndNeverUsedAsExitStatus) {
  const std::array values{
      Value::Integer(-1),
      Value::Integer(256),
      Value::Integer(std::numeric_limits<std::int64_t>::max()),
      Value::Double(1.5),
      Value::Boolean(true),
  };
  for (const auto value : values) {
    ProcessEntry process(isolate_, errors_);
    const auto type = Heap::TypeOf(value);
    EXPECT_EQ(process.Finish(value, &type), 0);
  }
  EXPECT_EQ(output_.str(), "-1\n256\n9223372036854775807\n1.5\ntrue\n");
  EXPECT_TRUE(errors_.str().empty());
}

TEST_F(ProcessEntryTest, VoidAndExplicitNullHaveDifferentOutput) {
  {
    ProcessEntry process(isolate_, errors_);
    EXPECT_EQ(process.Finish(Value::Null(), nullptr), 0);
  }
  EXPECT_TRUE(output_.str().empty());
  {
    ProcessEntry process(isolate_, errors_);
    const TypeConstraint nullable{.nullable = true};
    EXPECT_EQ(process.Finish(Value::Null(), &nullable), 0);
  }
  EXPECT_EQ(output_.str(), "null\n");
}

TEST_F(ProcessEntryTest, SynchronousCompletionDiscardsOnlyItsOwnTasks) {
  auto& scheduler = isolate_.scheduler();
  int other_calls = 0;
  scheduler.PostMicrotask([&] { ++other_calls; });
  auto other_timer = scheduler.After(std::chrono::milliseconds(0), [&] { ++other_calls; });
  int own_calls = 0;
  {
    ProcessEntry process(isolate_, errors_);
    scheduler.PostMicrotask([&] { ++own_calls; });
    const auto own_timer = scheduler.Periodic(std::chrono::milliseconds(0), [&] { ++own_calls; });
    EXPECT_EQ(process.Finish(Value::Integer(42), &integer_), 0);
  }
  while (scheduler.PumpOne() == FuturePumpStep::Executed) {
  }
  EXPECT_EQ(other_calls, 2);
  EXPECT_EQ(own_calls, 0);
  EXPECT_EQ(output_.str(), "42\n");
}

TEST_F(ProcessEntryTest, RootCompletionStopsWithinTheSameMicrotaskBatch) {
  auto& scheduler = isolate_.scheduler();
  ProcessEntry process(isolate_, errors_);
  Future root(&scheduler);
  const auto value = Wrap(root);
  int calls = 0;
  scheduler.PostMicrotask([&] {
    EXPECT_TRUE(root.Complete(Value::Integer(42)));
    ++calls;
    scheduler.PostMicrotask([&] { ++calls; });
    ++calls;
  });
  scheduler.PostMicrotask([&] { ++calls; });
  EXPECT_EQ(process.Finish(value, &integer_), 0);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(scheduler.PumpOne(), FuturePumpStep::Idle);
  EXPECT_EQ(output_.str(), "42\n");
}

TEST_F(ProcessEntryTest, FutureVoidAndFutureNullUseTheirPayloadTypes) {
  {
    ProcessEntry process(isolate_, errors_);
    const auto value = Wrap(Future::Value(Value::Null(), &isolate_.scheduler()), std::nullopt);
    EXPECT_EQ(process.Finish(value, &integer_), 0);
  }
  EXPECT_TRUE(output_.str().empty());
  {
    ProcessEntry process(isolate_, errors_);
    const auto value =
        Wrap(Future::Value(Value::Null(), &isolate_.scheduler()), TypeConstraint{.nullable = true});
    EXPECT_EQ(process.Finish(value, &integer_), 0);
  }
  EXPECT_EQ(output_.str(), "null\n");
}

TEST_F(ProcessEntryTest, FinishedDriverDoesNotCaptureSubsequentHostTasks) {
  ProcessEntry process(isolate_, errors_);
  EXPECT_EQ(process.Finish(Value::Integer(42), &integer_), 0);
  int calls = 0;
  isolate_.scheduler().PostMicrotask([&] { ++calls; });
  EXPECT_EQ(isolate_.scheduler().PumpOne(), FuturePumpStep::Executed);
  EXPECT_EQ(calls, 1);
}

TEST_F(ProcessEntryTest, PendingRootWaitsWithoutAProducerUntilExplicitStop) {
  ProcessEntry process(isolate_, errors_);
  const auto value = Wrap(Future(&isolate_.scheduler()));
  const auto endpoint = isolate_.native_control().endpoint();
  std::atomic_bool stopped = false;
  constexpr std::chrono::milliseconds kStopDelay{30};
  const std::jthread stop([&] {
    std::this_thread::sleep_for(kStopDelay);
    stopped = true;
    endpoint.RequestStop();
  });
  EXPECT_EQ(process.Finish(value, &integer_), 0);
  EXPECT_TRUE(stopped);
  EXPECT_TRUE(output_.str().empty());
  EXPECT_TRUE(errors_.str().empty());
}

TEST_F(ProcessEntryTest, CleanupClosesHostEndpointsAndReleasesPendingWaiters) {
  auto& scheduler = isolate_.scheduler();
  Future other(&scheduler);
  other.AttachHeap(isolate_.heap());
  FutureCompletionEndpoint endpoint;
  std::weak_ptr<int> released;
  int other_calls = 0;
  other.Observe([&](FutureState, Value) { ++other_calls; });
  {
    ProcessEntry process(isolate_, errors_);
    auto capture = std::make_shared<int>();
    released = capture;
    other.Observe([capture = std::move(capture)](FutureState, Value) {});
    auto completer = scheduler.CreateHostCompleter();
    auto host_future = completer.future();
    host_future.AttachHeap(isolate_.heap());
    endpoint = std::move(completer).Detach();
    EXPECT_EQ(process.Finish(Value::Integer(42), &integer_), 0);
    EXPECT_TRUE(released.expired());
  }
  EXPECT_EQ(endpoint.Complete(Value::Integer(1)).error(), FutureCompletionError::Closed);
  EXPECT_TRUE(other.Complete(Value::Integer(1)));
  EXPECT_EQ(scheduler.PumpOne(), FuturePumpStep::Executed);
  EXPECT_EQ(other_calls, 1);
}

TEST_F(ProcessEntryTest, RootFailureReportsOnceAndReturnsOne) {
  ProcessEntry process(isolate_, errors_);
  Future root(&isolate_.scheduler());
  const auto value = Wrap(root);
  EXPECT_TRUE(
      root.CompleteError(Value::Null(), {.message = "failed", .stack_trace = {}, .origin = {}}));
  EXPECT_EQ(process.Finish(value, &integer_), 1);
  EXPECT_TRUE(output_.str().empty());
  EXPECT_EQ(errors_.str(), "vela: async main failed: failed\n");
}

TEST_F(ProcessEntryTest, QueuedHostCompletionIsDiscardedAndCannotEnterANewRun) {
  auto& scheduler = isolate_.scheduler();
  FutureCompletionEndpoint endpoint;
  {
    ProcessEntry process(isolate_, errors_);
    auto completer = scheduler.CreateHostCompleter();
    auto future = completer.future();
    future.AttachHeap(isolate_.heap());
    endpoint = std::move(completer).Detach();
    std::jthread host([&] { EXPECT_TRUE(endpoint.CompleteText("late")); });
    host.join();
    EXPECT_EQ(process.Finish(Value::Integer(42), &integer_), 0);
  }
  {
    ProcessEntry next(isolate_, errors_);
    Future root(&scheduler);
    const auto value = Wrap(root);
    const auto timer = scheduler.After(std::chrono::milliseconds(0),
                                       [&] { EXPECT_TRUE(root.Complete(Value::Integer(7))); });
    const auto late = endpoint.CompleteText("later");
    ASSERT_FALSE(late);
    EXPECT_EQ(late.error(), FutureCompletionError::Closed);
    EXPECT_EQ(next.Finish(value, &integer_), 0);
  }
  EXPECT_EQ(output_.str(), "42\n7\n");
  EXPECT_TRUE(errors_.str().empty());
  EXPECT_EQ(scheduler.PumpOne(), FuturePumpStep::Idle);
}

TEST_F(ProcessEntryTest, ReportedBackgroundFailureMakesTheProgramFail) {
  ProcessEntry process(isolate_, errors_);
  const auto failed = Future::Error(Value::Null(), &isolate_.scheduler());
  Future root(&isolate_.scheduler());
  const auto value = Wrap(root);
  const auto timer = isolate_.scheduler().After(
      std::chrono::milliseconds(0), [&] { EXPECT_TRUE(root.Complete(Value::Integer(42))); });
  EXPECT_EQ(process.Finish(value, &integer_), 1);
  EXPECT_EQ(errors_.str(), "vela: unhandled Future error\n");
  EXPECT_TRUE(output_.str().empty());
}

TEST_F(ProcessEntryTest, ReleasingAWaiterCanCancelAnotherObservationOfTheSameFuture) {
  auto& scheduler = isolate_.scheduler();
  Future source(&scheduler);
  source.AttachHeap(isolate_.heap());
  auto subscribed = source.SubscribeResult([](const FutureResult&) {});
  ASSERT_TRUE(subscribed);
  auto owned = std::make_shared<FutureObservation>(std::move(*subscribed));
  const std::weak_ptr<FutureObservation> released = owned;
  {
    ProcessEntry process(isolate_, errors_);
    source.ObserveResult([owned = std::move(owned)](const FutureResult&) {});
    EXPECT_EQ(process.Finish(Value::Integer(42), &integer_), 0);
    EXPECT_TRUE(released.expired());
  }
  EXPECT_TRUE(source.Complete(Value::Integer(42)));
  EXPECT_EQ(scheduler.PumpOne(), FuturePumpStep::Idle);
}

TEST_F(ProcessEntryTest, OutputFailureMakesTheProgramFail) {
  ProcessEntry process(isolate_, errors_);
  output_.setstate(std::ios::badbit);
  EXPECT_EQ(process.Finish(Value::Integer(42), &integer_), 1);
  EXPECT_EQ(errors_.str(), "vela: error: failed to write program output\n");
}

TEST_F(ProcessEntryTest, ClosingAnObserverCanReleaseAnotherOwnedObserver) {
  auto& scheduler = isolate_.scheduler();
  Future source(&scheduler);
  source.AttachHeap(isolate_.heap());
  ProcessEntry process(isolate_, errors_);
  auto child = source.SubscribeResult([](const FutureResult&) {});
  ASSERT_TRUE(child);
  auto owned = std::make_shared<FutureObservation>(std::move(*child));
  const std::weak_ptr<FutureObservation> released = owned;
  const auto parent = source.SubscribeResult([owned = std::move(owned)](const FutureResult&) {});
  ASSERT_TRUE(parent);
  EXPECT_EQ(process.Finish(Value::Integer(42), &integer_), 0);
  EXPECT_TRUE(released.expired());
  EXPECT_TRUE(source.Complete(Value::Integer(42)));
  EXPECT_EQ(scheduler.PumpOne(), FuturePumpStep::Idle);
}

TEST_F(ProcessEntryTest, OtherRunsKeepTheirOwnUnhandledErrorHandler) {
  auto& scheduler = isolate_.scheduler();
  int other_errors = 0;
  scheduler.SetUnhandledErrorHandler([&](const Future&, Value) { ++other_errors; });
  const auto other = Future::Error(Value::Null(), &scheduler);
  {
    ProcessEntry process(isolate_, errors_);
    Future root(&scheduler);
    const auto value = Wrap(root);
    const auto timer = scheduler.After(std::chrono::milliseconds(0),
                                       [&] { EXPECT_TRUE(root.Complete(Value::Integer(42))); });
    EXPECT_EQ(process.Finish(value, &integer_), 0);
  }
  EXPECT_EQ(other_errors, 1);
  EXPECT_TRUE(errors_.str().empty());
  EXPECT_EQ(output_.str(), "42\n");
}

}  // namespace
}  // namespace vela::runtime
