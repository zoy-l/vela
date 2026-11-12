#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <vela/host.h>

#include "base/execution_status.h"
#include "runtime/async/future.h"
#include "runtime/isolate/isolate.h"

namespace vela {
namespace {

constexpr std::int64_t kAnswer = 42;

TEST(HostAsyncTest, CompleterPublishesManagedPayloadAndKeepsItAcrossGc) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto completer = CreateCompleter<String>(scope);
  ASSERT_TRUE(completer.has_value());
  const auto future = completer->GetFuture(scope);
  ASSERT_TRUE(future.has_value());
  EXPECT_EQ(future->State(), runtime::FutureState::Pending);
  const auto pending = future->Read(scope);
  ASSERT_FALSE(pending);
  EXPECT_EQ(pending.error().status, ExecutionStatus::AsyncIdle);
  {
    HandleScope child(isolate);
    const auto text = CreateString(child, "retained");
    ASSERT_TRUE(text.has_value());
    ASSERT_TRUE(completer->Complete(child, *text));
  }
  isolate.heap().Collect();
  const auto result = future->Read(scope);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->Utf8(), "retained");
  const auto duplicate = completer->Complete(scope, *result);
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().status, ExecutionStatus::FutureAlreadyCompleted);
}

TEST(HostAsyncTest, AdoptionSharesCompletionAndRejectsSelfAdoption) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto first = CreateCompleter<std::int64_t>(scope);
  const auto second = CreateCompleter<std::int64_t>(scope);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  const auto source = first->GetFuture(scope);
  const auto target = second->GetFuture(scope);
  ASSERT_TRUE(source.has_value());
  ASSERT_TRUE(target.has_value());
  const auto self = first->Adopt(scope, *source);
  ASSERT_FALSE(self);
  EXPECT_EQ(self.error().status, ExecutionStatus::FutureSelfAdoption);
  ASSERT_TRUE(second->Adopt(scope, *source));
  ASSERT_TRUE(first->Complete(scope, std::int64_t{kAnswer}));
  const auto result = target->Wait(scope);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, kAnswer);
}

TEST(HostAsyncTest, NullableAndVoidCompletionRetainTheirExactPayloadContracts) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto nullable = CreateCompleter<Nullable<String>>(scope);
  ASSERT_TRUE(nullable.has_value());
  const auto future = nullable->GetFuture(scope);
  ASSERT_TRUE(future.has_value());
  ASSERT_TRUE(nullable->Complete(scope, std::nullopt));
  const auto result = future->Read(scope);
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(*result);
  const auto empty = CreateCompleter<void>(scope);
  ASSERT_TRUE(empty.has_value());
  const auto done = empty->GetFuture(scope);
  ASSERT_TRUE(done.has_value());
  ASSERT_TRUE(empty->Complete(scope));
  EXPECT_TRUE(done->Read(scope));
}

TEST(HostAsyncTest, CompletionEndpointConstructsCopiedTextOnTheOwnerThread) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto pair = CreateCompletion<String>(scope);
  ASSERT_TRUE(pair.has_value());
  const auto endpoint = pair->completion;
  std::optional<Result<void>> posted;
  std::thread worker(
      [endpoint, &posted] { posted = endpoint.Complete(std::string("from worker")); });
  worker.join();
  if (!posted) {
    FAIL() << "expected a completion result from the worker";
  }
  ASSERT_TRUE(*posted);
  const auto result = pair->future.Wait(scope, WaitMode::WaitForHost);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->Utf8(), "from worker");
  const auto duplicate = endpoint.Complete(std::string("again"));
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().status, ExecutionStatus::FutureAlreadyCompleted);
}

TEST(HostAsyncTest, CompletionEndpointRejectsForeignPayloadAndRemainsUsable) {
  runtime::Isolate first(1);
  runtime::Isolate second(1);
  HandleScope target(first);
  HandleScope foreign(second);
  const auto pair = CreateCompletion<String>(target);
  ASSERT_TRUE(pair.has_value());
  const auto text = CreateString(foreign, "wrong");
  ASSERT_TRUE(text.has_value());
  const auto rejected = pair->completion.Complete(foreign, *text);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, ExecutionStatus::InvalidIsolate);
  const auto correct = CreateString(target, "correct");
  ASSERT_TRUE(correct.has_value());
  ASSERT_TRUE(pair->completion.Complete(target, *correct));
  const auto result = pair->future.Wait(target);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->Utf8(), "correct");
}

TEST(HostAsyncTest, FailurePreservesStatusAndDiagnosticInsteadOfBecomingATypeError) {
  runtime::Isolate isolate;
  HandleScope scope(isolate);
  const auto pair = CreateCompletion<String>(scope);
  ASSERT_TRUE(pair.has_value());
  ASSERT_TRUE(pair->completion.Fail("failed in host"));
  const auto result = pair->future.Wait(scope);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().status, ExecutionStatus::NativeFailure);
  EXPECT_EQ(result.error().message, "failed in host");
}

TEST(HostAsyncTest, EndpointsOutliveShutdownAndQueuedTextDoesNotKeepTheIsolateAlive) {
  std::optional<Completion<String>> endpoint;
  {
    runtime::Isolate isolate;
    {
      HandleScope scope(isolate);
      auto pair = CreateCompletion<String>(scope);
      ASSERT_TRUE(pair.has_value());
      endpoint = pair->completion;
      ASSERT_TRUE(endpoint->Complete(std::string("queued")));
    }
    ASSERT_TRUE(isolate.TryStop());
  }
  const auto closed = endpoint->Complete(std::string("late"));
  ASSERT_FALSE(closed);
  EXPECT_EQ(closed.error().status, ExecutionStatus::ExecutionStopped);
}

TEST(HostAsyncTest, ObservationRootsTheResultUntilDeliveryAndReleasesItsCapture) {
  runtime::Isolate isolate(1);
  std::optional<FutureObservation> observation;
  std::optional<Completion<String>> endpoint;
  Persistent<String> saved;
  auto resource = std::make_shared<int>(kAnswer);
  const std::weak_ptr<int> weak = resource;
  {
    HandleScope scope(isolate);
    const auto pair = CreateCompletion<String>(scope);
    ASSERT_TRUE(pair.has_value());
    endpoint = pair->completion;
    auto subscribed = ObserveFuture(
        scope, pair->future,
        [resource = std::move(resource), &saved](HandleScope&, Result<Local<String>> result) {
          ASSERT_TRUE(result.has_value());
          EXPECT_EQ(*resource, kAnswer);
          EXPECT_EQ(result->Utf8(), "delivered");
          auto root = result->Persist();
          ASSERT_TRUE(root.has_value());
          saved = std::move(*root);
        });
    ASSERT_TRUE(subscribed.has_value());
    observation = std::move(*subscribed);
  }
  isolate.heap().Collect();
  EXPECT_FALSE(weak.expired());
  ASSERT_TRUE(endpoint->Complete(std::string("delivered")));
  ASSERT_EQ(isolate.scheduler().PumpOne(), runtime::FuturePumpStep::Executed);
  EXPECT_TRUE(weak.expired());
  ASSERT_FALSE(saved.IsEmpty());
  {
    HandleScope scope(isolate);
    const auto text = saved.ToLocal(scope);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->Utf8(), "delivered");
  }
  saved.Reset();
  observation.reset();
  endpoint.reset();
  isolate.heap().Collect();
  EXPECT_TRUE(isolate.ReloadBlockers().empty());
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

TEST(HostAsyncTest, CancellingPendingAndQueuedObservationsReleasesResourcesImmediately) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto completer = CreateCompleter<void>(scope);
  ASSERT_TRUE(completer.has_value());
  const auto future = completer->GetFuture(scope);
  ASSERT_TRUE(future.has_value());
  for (const auto completed : {false, true}) {
    if (completed) {
      ASSERT_TRUE(completer->Complete(scope));
    }
    auto resource = std::make_unique<int>(kAnswer);
    int calls = 0;
    auto observer = ObserveFuture(
        scope, *future, [resource = std::move(resource), &calls](HandleScope&, Result<void>) {
          calls += *resource;
        });
    ASSERT_TRUE(observer.has_value());
    observer->Cancel();
    std::ignore = isolate.scheduler().PumpOne();
    EXPECT_EQ(calls, 0);
  }
  auto resource = std::make_shared<int>(kAnswer);
  const std::weak_ptr<int> weak = resource;
  {
    auto observer = ObserveFuture(scope, *future,
                                  [resource = std::move(resource)](HandleScope&, Result<void>) {});
    ASSERT_TRUE(observer.has_value());
  }
  EXPECT_TRUE(weak.expired());
}

TEST(HostAsyncTest, ShutdownClosesPendingObservationsAndTokensCanBeReleasedLater) {
  std::optional<FutureObservation> observation;
  auto resource = std::make_shared<int>(kAnswer);
  const std::weak_ptr<int> weak = resource;
  {
    runtime::Isolate isolate(1);
    {
      HandleScope scope(isolate);
      const auto completer = CreateCompleter<void>(scope);
      ASSERT_TRUE(completer.has_value());
      const auto future = completer->GetFuture(scope);
      ASSERT_TRUE(future.has_value());
      auto subscribed = ObserveFuture(scope, *future,
                                      [resource = std::move(resource)](HandleScope&, Result<void>) {
                                        ADD_FAILURE() << "closed observation was called";
                                      });
      ASSERT_TRUE(subscribed.has_value());
      observation = std::move(*subscribed);
    }
    isolate.heap().Collect();
    EXPECT_FALSE(weak.expired());
    ASSERT_TRUE(isolate.TryStop());
    EXPECT_TRUE(weak.expired());
  }
  observation.reset();
}

TEST(HostAsyncTest, ObservationsPreserveFailurePayloadAndReportAllocationFailure) {
  runtime::Isolate isolate(1);
  HandleScope scope(isolate);
  const auto completer = CreateCompleter<String>(scope);
  ASSERT_TRUE(completer.has_value());
  const auto future = completer->GetFuture(scope);
  ASSERT_TRUE(future.has_value());
  isolate.heap().SetAllocationLimit(isolate.heap().allocated_bytes());
  const auto rejected = ObserveFuture(scope, *future, [](HandleScope&, Result<Local<String>>) {});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().status, ExecutionStatus::OutOfMemory);
  isolate.heap().SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  std::optional<Error> saved;
  auto observer =
      ObserveFuture(scope, *future, [&saved](HandleScope&, Result<Local<String>> result) {
        ASSERT_FALSE(result);
        saved = std::move(result.error());
      });
  ASSERT_TRUE(observer.has_value());
  ASSERT_TRUE(completer->Fail(scope, {.status = ExecutionStatus::NativeFailure,
                                      .message = "observed failure",
                                      .stack_trace = {}}));
  ASSERT_EQ(isolate.scheduler().PumpOne(), runtime::FuturePumpStep::Executed);
  if (!saved) {
    FAIL() << "expected the observer to receive the failure";
  }
  EXPECT_EQ(saved->message, "observed failure");
  EXPECT_EQ(saved->status, ExecutionStatus::NativeFailure);
}

TEST(HostAsyncTest, CancellationSourceSharesOnlyCancellationStateAcrossThreads) {
  runtime::Isolate isolate(1);
  CancellationSource source;
  {
    HandleScope scope(isolate);
    const auto token = CreateCancellationToken(scope);
    ASSERT_TRUE(token.has_value());
    EXPECT_FALSE(IsCancelled(*token));
    source = GetCancellationSource(*token);
  }
  isolate.heap().Collect();
  std::thread worker([source] mutable { source.Cancel(); });
  worker.join();
  HandleScope scope(isolate);
  const auto token = source.ToLocal(scope);
  ASSERT_TRUE(token.has_value());
  EXPECT_TRUE(IsCancelled(*token));
  Cancel(*token);
  EXPECT_TRUE(source.IsCancelled());
}

}  // namespace
}  // namespace vela
