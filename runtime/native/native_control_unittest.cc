#include "runtime/native/native_control.h"

#include <optional>
#include <thread>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "runtime/native/native_debugger.h"

namespace vela::runtime {
namespace {

TEST(NativeControlTest, SafepointRequestsPreserveIndependentSourcesUntilEachIsConsumed) {
  NativeControl control;
  const auto& requests = control.safepoint_requests();
  EXPECT_EQ(requests.load(), 0);
  int calls = 0;
  ASSERT_TRUE(control.ScheduleSafepoint([&] { ++calls; }));
  const auto endpoint = control.endpoint();
  ASSERT_TRUE(endpoint.Post([&] { ++calls; }));
  ASSERT_TRUE(endpoint.Post([&] { ++calls; }));
  EXPECT_NE(requests.load(), 0);
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
  EXPECT_EQ(calls, 1);
  EXPECT_NE(requests.load(), 0);
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
  EXPECT_EQ(calls, 2);
  EXPECT_NE(requests.load(), 0);
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
  EXPECT_EQ(calls, 3);
  EXPECT_EQ(requests.load(), 0);

  ASSERT_TRUE(control.ScheduleSafepoint([&] { ++calls; }));
  std::jthread request([&] { endpoint.RequestStop(); });
  request.join();
  control.CancelSafepoint();
  EXPECT_NE(requests.load(), 0);
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::ExecutionStopped);
  EXPECT_EQ(calls, 3);
}

TEST(NativeControlTest, DetachingDebuggerDoesNotClearQueuedOrScheduledWork) {
  class Debugger final : public NativeDebugger {
   public:
    void AtSafepoint(const NativeDebugContext& /*context*/) noexcept override {}
  } debugger;
  NativeControl control;
  const auto& requests = control.safepoint_requests();
  control.SetDebugger(&debugger);
  EXPECT_NE(requests.load(), 0);
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
  EXPECT_NE(requests.load(), 0);
  int calls = 0;
  ASSERT_TRUE(control.endpoint().Post([&] { ++calls; }));
  ASSERT_TRUE(control.ScheduleSafepoint([&] { ++calls; }));
  control.SetDebugger(nullptr);
  EXPECT_NE(requests.load(), 0);
  control.CancelSafepoint();
  EXPECT_NE(requests.load(), 0);
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(requests.load(), 0);
}

TEST(NativeControlTest, StopWaitsForTheOutermostHostBorrowToReturn) {
  NativeControl control;
  control.EnterHost();
  control.EnterHost();
  control.RequestStop();
  EXPECT_EQ(control.Poll(), ExecutionStatus::Returned);
  control.ExitHost();
  EXPECT_EQ(control.Poll(), ExecutionStatus::Returned);
  control.ExitHost();
  EXPECT_EQ(control.Poll(), ExecutionStatus::ExecutionStopped);
}

TEST(NativeControlTest, StopCanBeRequestedWithoutBorrowingTheIsolateFromAnotherThread) {
  NativeControl control;
  EXPECT_EQ(control.Poll(), ExecutionStatus::Returned);
  std::jthread request([&] { control.RequestStop(); });
  request.join();
  EXPECT_EQ(control.Poll(), ExecutionStatus::ExecutionStopped);
  EXPECT_FALSE(control.CanPublish());
}

TEST(NativeControlTest, PublicationWaitsForNestedInitializationsAndHostReentry) {
  NativeControl control;
  EXPECT_TRUE(control.CanPublish());
  {
    const NativeInitializationScope outer(control);
    EXPECT_FALSE(control.CanPublish());
    {
      const NativeInitializationScope inner(control);
      control.EnterHost();
      EXPECT_FALSE(control.CanPublish());
    }
    control.ExitHost();
    EXPECT_FALSE(control.CanPublish());
  }
  EXPECT_TRUE(control.CanPublish());
}

TEST(NativeControlTest, PendingPublicationWaitsForHostAndInitializationAndRunsOnce) {
  NativeControl control;
  int calls = 0;
  ASSERT_TRUE(control.ScheduleSafepoint([&] { ++calls; }));
  control.EnterHost();
  control.EnterInitialization();
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
  EXPECT_EQ(calls, 0);
  control.ExitHost();
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
  EXPECT_EQ(calls, 0);
  control.ExitInitialization();
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
  EXPECT_EQ(calls, 1);
}

TEST(NativeControlTest, NestedSafepointsDoNotConsumeTheNextPublicationReentrantly) {
  NativeControl control;
  int calls = 0;
  ASSERT_TRUE(control.ScheduleSafepoint([&] {
    ++calls;
    ASSERT_TRUE(control.ScheduleSafepoint([&] { ++calls; }));
    EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
    EXPECT_EQ(calls, 1);
  }));
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
  EXPECT_EQ(calls, 2);
}

TEST(NativeControlTest, PendingPublicationCanBeCancelledAndCannotRunAfterStop) {
  NativeControl control;
  int calls = 0;
  ASSERT_TRUE(control.ScheduleSafepoint([&] { ++calls; }));
  const auto duplicate = control.ScheduleSafepoint([] {});
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error(), NativeSafepointError::AlreadyPending);
  control.CancelSafepoint();
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
  EXPECT_EQ(calls, 0);
  ASSERT_TRUE(control.ScheduleSafepoint([&] { ++calls; }));
  control.RequestStop();
  EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::ExecutionStopped);
  EXPECT_EQ(calls, 0);
  const auto stopped = control.ScheduleSafepoint([] {});
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error(), NativeSafepointError::Stopped);
}

TEST(NativeControlTest, CrossThreadActionsWaitForOwnerSafepointsAndOldEndpointsClose) {
  std::optional<NativeSafepointEndpoint> endpoint;
  std::thread::id executed;
  const auto owner = std::this_thread::get_id();
  {
    NativeControl control;
    endpoint = control.endpoint();
    control.EnterHost();
    std::thread producer([&] {
      const auto queued = endpoint->Post([&] { executed = std::this_thread::get_id(); });
      EXPECT_TRUE(queued.has_value());
    });
    producer.join();
    EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
    EXPECT_EQ(executed, std::thread::id{});
    control.ExitHost();
    EXPECT_EQ(control.ProcessSafepoint(), ExecutionStatus::Returned);
    EXPECT_EQ(executed, owner);
  }
  const auto closed = endpoint->Post([] {});
  ASSERT_FALSE(closed);
  EXPECT_EQ(closed.error(), NativeSafepointError::Stopped);
}

}  // namespace
}  // namespace vela::runtime
