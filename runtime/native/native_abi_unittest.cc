#include "runtime/native/native_abi.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "base/source_location.h"
#include "runtime/exceptions/exception.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_api.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"

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
      Value::Null(),
      Value::Boolean(false),
      Value::Boolean(true),
      Value::Integer(std::numeric_limits<std::int64_t>::min()),
      Value::Integer(std::numeric_limits<std::int64_t>::max()),
      Value::Double(-0.0),
      Value::Double(nan),
      Value::Double(std::numeric_limits<double>::infinity()),
      Value::Object(object),
      Value::String(string),
  });
  for (const auto value : values) {
    auto encoded = EncodeNativeValue(value);
    EXPECT_EQ(encoded.reserved_byte, 0U);
    EXPECT_EQ(encoded.reserved_half, 0U);
    EXPECT_EQ(encoded.reserved_word, 0U);
    const auto expected = encoded;
    // ABI 保留字段不属于 Value；解码不得将其作为载荷或标签消费。
    encoded.reserved_byte = std::numeric_limits<std::uint8_t>::max();
    encoded.reserved_half = std::numeric_limits<std::uint16_t>::max();
    encoded.reserved_word = std::numeric_limits<std::uint32_t>::max();
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
  NativeFrame frame(isolate, {.root_count = 1});
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
  NativeFrame roots(isolate, {.root_count = std::numeric_limits<std::size_t>::max()});
  NativeFrame storage(isolate, {.storage_bytes = std::numeric_limits<std::size_t>::max()});
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
    NativeFrame resources(isolate, {.root_count = 1});
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

TEST(NativeAbiTest, ResourceEntryOwnsRootsAndReleasesStorage) {
  constexpr std::uint64_t kStorageBytes = 128;
  Isolate isolate;
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  alignas(NativeFrame) std::array<std::byte, sizeof(NativeFrame)> storage{};
  void* frame_storage = storage.data();
  auto* frame = static_cast<NativeFrame*>(frame_storage);
  NativeFrameAccess outer;
  ASSERT_EQ(vela_native_v1_frame_storage(&execution, 1, kStorageBytes, frame, &outer),
            static_cast<std::uint8_t>(ExecutionStatus::Returned));
  ASSERT_NE(outer.roots, nullptr);
  EXPECT_TRUE(outer.roots->IsNull());
  ASSERT_NE(outer.storage, nullptr);
  EXPECT_EQ(outer.storage, frame->storage());
  vela_native_v1_frame_release(frame);
  EXPECT_EQ(execution.frame_count(), 0U);
}

TEST(NativeAbiTest, BorrowedStackRootsUseOnlyRootRangeAndReleaseTheirReferences) {
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto image = EmptyImage();
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  const auto baseline = heap.object_count();
  alignas(RootRange) std::array<std::byte, sizeof(RootRange)> storage{};
  void* range_storage = storage.data();
  auto* range = static_cast<RootRange*>(range_storage);
  std::array<Value, 1> roots;
  vela_native_v1_roots_enter(&execution, roots.data(), roots.size(), range);
  auto* text = heap.AllocateString("stack rooted");
  ASSERT_NE(text, nullptr);
  roots.front() = Value::String(text);
  heap.Collect();
  EXPECT_EQ(text->value(), "stack rooted");
  EXPECT_EQ(heap.object_count(), baseline + 1);
  vela_native_v1_roots_exit(range);
  heap.Collect();
  EXPECT_EQ(heap.object_count(), baseline);
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

TEST(NativeAbiTest, StoppedExecutionHasPublicNativeDiagnostic) {
  EXPECT_STREQ(
      vela_aot_v1_status_code(static_cast<VelaAotStatus>(ExecutionStatus::ExecutionStopped)),
      "ExecutionStopped");
}

TEST(NativeAbiTest, LeafFailureRetainsBorrowedRootsDuringExceptionAllocationAndReleasesThem) {
  Isolate isolate(1);
  auto image = EmptyImage();
  std::array<NativeFunctionDescription, 2> functions;
  const std::array<SourceLocation, 8> locations;
  functions.back().locations = {.data = locations.data(), .size = locations.size()};
  const std::array<NativeText, 1> paths;
  image.source_paths = {.data = paths.data(), .size = paths.size()};
  image.functions = {.data = functions.data(), .size = functions.size()};
  const auto instance = NativeInstance::Instantiate(isolate, image);
  ASSERT_TRUE(instance.has_value());
  NativeExecution execution(**instance);
  auto& heap = isolate.heap();
  heap.Collect();
  const auto baseline = heap.object_count();
  auto* string = heap.AllocateString("retained only by the cold leaf root range");
  ASSERT_NE(string, nullptr);
  const std::array roots{Value::String(string)};
  const TracePosition position{.function = 1, .instruction = 7};
  EXPECT_EQ(static_cast<ExecutionStatus>(vela_native_v2_leaf_failure(
                &execution, EncodeNativeTracePosition(position),
                static_cast<std::uint8_t>(ExecutionStatus::IndexOutOfBounds), roots.data(),
                roots.size())),
            ExecutionStatus::IndexOutOfBounds);
  EXPECT_EQ(string->value(), "retained only by the cold leaf root range");
  EXPECT_EQ(execution.frame_count(), 0U);
  ASSERT_TRUE(execution.HasException());
  const auto& error = ExceptionData::From(*execution.pending_exception().AsObject());
  const auto diagnostic = execution.ExceptionDiagnostic();
  ASSERT_EQ(diagnostic.stack_trace.size(), 1U);
  EXPECT_EQ(diagnostic.stack_trace.front().instruction, position.instruction);
  EXPECT_EQ(error.trace().size(), 0U);
  static_cast<void>(execution.TakeException());
  heap.Collect();
  EXPECT_EQ(heap.object_count(), baseline);
}

}  // namespace vela::runtime
