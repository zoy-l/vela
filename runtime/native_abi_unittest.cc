#include "runtime/native_abi.h"

#include <cstddef>
#include <limits>

#include <gtest/gtest.h>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "runtime/isolate.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {

TEST(NativeAbiTest, RootsSurviveCollectionAndFramesReleaseThem) {
  Isolate isolate;
  auto& heap = isolate.heap();
  NativeExecution execution(isolate);
  ASSERT_EQ(execution.Enter(1), ExecutionStatus::Returned);
  auto* string = heap.AllocateString("rooted");
  ASSERT_NE(string, nullptr);
  execution.current_roots().front() = Value::String(string);
  heap.Collect();
  EXPECT_EQ(string->value(), "rooted");
  execution.Exit();
  EXPECT_EQ(execution.frame_count(), 0U);
}

TEST(NativeAbiTest, CallDepthAndAllocationFailuresPreserveExistingFrames) {
  Isolate isolate;
  NativeExecution execution(isolate);
  EXPECT_EQ(execution.Enter(std::numeric_limits<std::size_t>::max()), ExecutionStatus::OutOfMemory);
  EXPECT_EQ(execution.frame_count(), 0U);
  for (std::size_t index = 0; index < kMaxCallFrames; ++index) {
    ASSERT_EQ(execution.Enter(0), ExecutionStatus::Returned);
  }
  EXPECT_EQ(execution.Enter(0), ExecutionStatus::StackOverflow);
  EXPECT_EQ(execution.frame_count(), kMaxCallFrames);
}

}  // namespace vela::runtime
