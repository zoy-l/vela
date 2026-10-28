#include "runtime/native_abi.h"

#include <cstddef>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "runtime/isolate.h"
#include "runtime/native_image.h"
#include "runtime/native_instance.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

NativeImage EmptyImage() {
  NativeImage image;
  const auto abi = NativeRuntimeAbiIdentity();
  const auto target = NativeTargetIdentity();
  image.abi_identity = {.data = abi.data(), .size = abi.size()};
  image.target = {.data = target.data(), .size = target.size()};
  return image;
}

}  // namespace

TEST(NativeAbiTest, RootsSurviveCollectionAndFramesReleaseThem) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  ASSERT_EQ(execution.Enter(0, {.root_count = 1}), ExecutionStatus::Returned);
  auto* string = heap.AllocateString("rooted");
  ASSERT_NE(string, nullptr);
  execution.current_roots().front() = Value::String(string);
  heap.Collect();
  EXPECT_EQ(string->value(), "rooted");
  execution.Exit();
  EXPECT_EQ(execution.frame_count(), 0U);
}

TEST(NativeAbiTest, AllocationFailuresPreserveExistingFrames) {
  Isolate isolate;
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  EXPECT_EQ(execution.Enter(0, {.root_count = std::numeric_limits<std::size_t>::max()}),
            ExecutionStatus::OutOfMemory);
  EXPECT_EQ(execution.Enter(0, {.storage_bytes = std::numeric_limits<std::size_t>::max()}),
            ExecutionStatus::OutOfMemory);
  EXPECT_EQ(execution.frame_count(), 0U);
}

TEST(NativeAbiTest, CallDepthLimitPreservesExistingFrames) {
  Isolate isolate;
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  for (std::size_t index = 0; index < kMaxCallFrames; ++index) {
    ASSERT_EQ(execution.Enter(0, {}), ExecutionStatus::Returned);
  }
  EXPECT_EQ(execution.Enter(0, {}), ExecutionStatus::StackOverflow);
  EXPECT_EQ(vela_native_v1_leaf_enter(&execution),
            static_cast<std::uint8_t>(ExecutionStatus::StackOverflow));
  EXPECT_EQ(execution.frame_count(), kMaxCallFrames);
}

TEST(NativeAbiTest, ReusedFramesKeepOuterPositionsAndReleasePreviousRoots) {
  constexpr std::uint32_t kOuterInstruction = 42;
  constexpr std::uint32_t kInnerInstruction = 7;
  Isolate isolate;
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  ASSERT_EQ(execution.Enter(0, {.root_count = 1}), ExecutionStatus::Returned);
  auto* outer_position = execution.current_instruction();
  *outer_position = kOuterInstruction;
  auto& heap = isolate.heap();
  const auto baseline = heap.object_count();
  ASSERT_EQ(execution.Enter(1, {.root_count = 1}), ExecutionStatus::Returned);
  auto* text = heap.AllocateString("temporary");
  ASSERT_NE(text, nullptr);
  execution.current_roots().front() = Value::String(text);
  *execution.current_instruction() = kInnerInstruction;
  heap.Collect();
  EXPECT_EQ(heap.object_count(), baseline + 1);
  EXPECT_EQ(*outer_position, kOuterInstruction);
  execution.Exit();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), baseline);
  ASSERT_EQ(execution.Enter(2, {}), ExecutionStatus::Returned);
  EXPECT_EQ(*execution.current_instruction(), 0U);
  EXPECT_TRUE(execution.current_roots().empty());
  execution.Exit();
  EXPECT_EQ(execution.current_instruction(), outer_position);
  EXPECT_EQ(*outer_position, kOuterInstruction);
  execution.Exit();
}

TEST(NativeAbiTest, EntryBorrowsStablePositionsRootsAndStorageTogether) {
  constexpr std::uint32_t kOuterInstruction = 19;
  constexpr std::uint64_t kStorageBytes = 128;
  Isolate isolate;
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  NativeFrameAccess outer;
  ASSERT_EQ(vela_native_v1_frame_enter(&execution, 1, 0, kStorageBytes, &outer),
            static_cast<std::uint8_t>(ExecutionStatus::Returned));
  EXPECT_EQ(outer.instruction, execution.current_instruction());
  EXPECT_EQ(outer.roots, execution.current_roots().data());
  ASSERT_NE(outer.storage, nullptr);
  EXPECT_EQ(outer.storage, execution.current_storage());
  *outer.instruction = kOuterInstruction;
  NativeFrameAccess scalar;
  ASSERT_EQ(vela_native_v1_frame_enter(&execution, 0, 1, 0, &scalar),
            static_cast<std::uint8_t>(ExecutionStatus::Returned));
  EXPECT_NE(scalar.instruction, outer.instruction);
  EXPECT_EQ(scalar.roots, nullptr);
  EXPECT_EQ(scalar.storage, nullptr);
  vela_native_v1_frame_exit(&execution);
  EXPECT_EQ(execution.current_instruction(), outer.instruction);
  EXPECT_EQ(*outer.instruction, kOuterInstruction);
  EXPECT_EQ(execution.current_roots().data(), outer.roots);
  vela_native_v1_frame_exit(&execution);
  EXPECT_EQ(execution.frame_count(), 0U);
}

TEST(NativeAbiTest, ScalarLeafEntryDoesNotMaterializeAFrame) {
  Isolate isolate;
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  EXPECT_EQ(vela_native_v1_leaf_enter(&execution),
            static_cast<std::uint8_t>(ExecutionStatus::Returned));
  EXPECT_EQ(execution.frame_count(), 0U);
}

}  // namespace vela::runtime
