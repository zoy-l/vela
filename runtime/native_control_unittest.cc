#include "runtime/native_control.h"

#include <thread>

#include <gtest/gtest.h>

#include "base/execution_status.h"

namespace vela::runtime {
namespace {

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

}  // namespace
}  // namespace vela::runtime
