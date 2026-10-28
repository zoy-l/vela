#include "runtime/native_abi.h"

#include <array>
#include <bit>
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

TEST(NativeAbiTest, ValueCodecsPreservePayloadBitsAndClearReservedFields) {
  Isolate isolate;
  auto* object = isolate.heap().AllocateValueSlots(1);
  auto* string = isolate.heap().AllocateString("payload");
  ASSERT_NE(object, nullptr);
  ASSERT_NE(string, nullptr);
  const auto nan = std::bit_cast<double>(std::uint64_t{0x7ff8000000000042});
  const auto values = std::to_array<Value>({
      Value::Null(), Value::Boolean(false), Value::Boolean(true),
      Value::Integer(std::numeric_limits<std::int64_t>::min()),
      Value::Integer(std::numeric_limits<std::int64_t>::max()),
      Value::Double(-0.0), Value::Double(nan),
      Value::Double(std::numeric_limits<double>::infinity()),
      Value::Object(object), Value::String(string),
  });
  for (const auto value : values) {
    auto encoded = EncodeNativeValue(value);
    EXPECT_EQ(encoded.reserved_byte, 0U);
    EXPECT_EQ(encoded.reserved_half, 0U);
    EXPECT_EQ(encoded.reserved_word, 0U);
    const auto expected = encoded;
    // ABI 保留字段不属于 Value；解码不得将其作为载荷或标签消费。
    encoded.reserved_byte = 0xff;
    encoded.reserved_half = 0xffff;
    encoded.reserved_word = 0xffffffff;
    const auto decoded = DecodeNativeValue(encoded);
    EXPECT_EQ(decoded.kind(), value.kind());
    const auto roundtrip = EncodeNativeValue(decoded);
    EXPECT_EQ(roundtrip.kind, expected.kind);
    EXPECT_EQ(roundtrip.payload, expected.payload);
    EXPECT_EQ(roundtrip.reserved_byte, 0U);
    EXPECT_EQ(roundtrip.reserved_half, 0U);
    EXPECT_EQ(roundtrip.reserved_word, 0U);
  }
}

TEST(NativeAbiTest, RootsSurviveCollectionAndFramesReleaseThem) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  NativeTraceFrame trace;
  ASSERT_EQ(execution.Enter(trace, 0), ExecutionStatus::Returned);
  std::array<Value, 1> roots;
  NativeFrame frame(isolate, {.root_count = 1}, roots.data());
  auto* string = heap.AllocateString("rooted");
  ASSERT_NE(string, nullptr);
  frame.roots().front() = Value::String(string);
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
  NativeFrame roots(isolate, {.root_count = std::numeric_limits<std::size_t>::max()}, nullptr);
  NativeFrame storage(isolate, {.storage_bytes = std::numeric_limits<std::size_t>::max()}, nullptr);
  EXPECT_FALSE(roots.IsAllocated());
  EXPECT_FALSE(storage.IsAllocated());
  EXPECT_EQ(execution.frame_count(), 0U);
}

TEST(NativeAbiTest, CallDepthLimitPreservesExistingFrames) {
  Isolate isolate;
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  std::array<NativeTraceFrame, kMaxCallFrames> frames;
  for (auto& frame : frames) {
    ASSERT_EQ(execution.Enter(frame, 0), ExecutionStatus::Returned);
  }
  NativeTraceFrame overflow;
  EXPECT_EQ(execution.Enter(overflow, 0), ExecutionStatus::StackOverflow);
  EXPECT_EQ(execution.PrepareEntry(), ExecutionStatus::StackOverflow);
  EXPECT_EQ(execution.frame_count(), kMaxCallFrames);
  while (execution.frame_count() != 0) {
    execution.Exit();
  }
}

TEST(NativeAbiTest, ReusedFramesKeepOuterPositionsAndReleasePreviousRoots) {
  constexpr std::uint32_t kOuterInstruction = 42;
  constexpr std::uint32_t kInnerInstruction = 7;
  Isolate isolate;
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  NativeTraceFrame outer;
  NativeTraceFrame inner;
  ASSERT_EQ(execution.Enter(outer, 0), ExecutionStatus::Returned);
  auto* outer_position = execution.current_instruction();
  *outer_position = kOuterInstruction;
  auto& heap = isolate.heap();
  const auto baseline = heap.object_count();
  ASSERT_EQ(execution.Enter(inner, 1), ExecutionStatus::Returned);
  {
    NativeFrame resources(isolate, {.root_count = 1}, nullptr);
    auto* text = heap.AllocateString("temporary");
    ASSERT_NE(text, nullptr);
    resources.roots().front() = Value::String(text);
    *execution.current_instruction() = kInnerInstruction;
    heap.Collect();
    EXPECT_EQ(heap.object_count(), baseline + 1);
    EXPECT_EQ(*outer_position, kOuterInstruction);
    execution.Exit();
  }
  heap.Collect();
  EXPECT_EQ(heap.object_count(), baseline);
  ASSERT_EQ(execution.Enter(inner, 2), ExecutionStatus::Returned);
  EXPECT_EQ(*execution.current_instruction(), 0U);
  execution.Exit();
  EXPECT_EQ(execution.current_instruction(), outer_position);
  EXPECT_EQ(*outer_position, kOuterInstruction);
  execution.Exit();
}

TEST(NativeAbiTest, ResourceEntryBorrowsStackRootsAndReleasesStorage) {
  constexpr std::uint64_t kStorageBytes = 128;
  Isolate isolate;
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  alignas(NativeFrame) std::array<std::byte, sizeof(NativeFrame)> storage;
  auto* frame = reinterpret_cast<NativeFrame*>(storage.data());
  std::array<Value, 1> roots;
  NativeFrameAccess outer;
  ASSERT_EQ(vela_native_v1_frame_storage(&execution, 1, kStorageBytes, roots.data(), frame, &outer),
            static_cast<std::uint8_t>(ExecutionStatus::Returned));
  EXPECT_EQ(outer.roots, roots.data());
  ASSERT_NE(outer.storage, nullptr);
  EXPECT_EQ(outer.storage, frame->storage());
  vela_native_v1_frame_release(frame);
  EXPECT_EQ(execution.frame_count(), 0U);
}

TEST(NativeAbiTest, ScalarLeafEntryDoesNotMaterializeAFrame) {
  Isolate isolate;
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  EXPECT_EQ(execution.PrepareEntry(), ExecutionStatus::Returned);
  EXPECT_EQ(execution.frame_count(), 0U);
}

}  // namespace vela::runtime
