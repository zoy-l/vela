#include "runtime/heap/native_buffer.h"

#include <cstddef>
#include <limits>

#include <gtest/gtest.h>

#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"

namespace vela::runtime {
namespace {

TEST(NativeBufferTest, InlineAndEmptyBuffersDoNotConsumeTheHeapBudget) {
  constexpr std::size_t kInlineCapacity = 4;
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto bytes = heap.allocated_bytes();
  heap.SetAllocationLimit(bytes);
  NativeBuffer<std::size_t, kInlineCapacity> inline_buffer(heap, kInlineCapacity);
  NativeBuffer<char> empty(heap, 0);
  NativeBuffer<std::size_t, kInlineCapacity> rejected(heap, kInlineCapacity + 1);
  EXPECT_TRUE(inline_buffer.IsAllocated());
  EXPECT_EQ(inline_buffer.values().size(), kInlineCapacity);
  EXPECT_TRUE(empty.IsAllocated());
  EXPECT_TRUE(empty.values().empty());
  EXPECT_FALSE(rejected.IsAllocated());
  EXPECT_EQ(heap.allocated_bytes(), bytes);
}

TEST(NativeBufferTest, ExternalStorageIsAccountedAndOverflowIsRejected) {
  constexpr std::size_t kCount = 8;
  constexpr std::size_t kValue = 42;
  Isolate isolate;
  auto& heap = isolate.heap();
  const auto bytes = heap.allocated_bytes();
  const auto committed = heap.committed_bytes();
  {
    NativeBuffer<std::size_t> buffer(heap, kCount);
    ASSERT_TRUE(buffer.IsAllocated());
    EXPECT_GE(heap.allocated_bytes() - bytes, kCount * sizeof(std::size_t));
    buffer.values().front() = kValue;
    EXPECT_EQ(buffer.values().front(), kValue);
    NativeBuffer<std::size_t> overflow(heap, std::numeric_limits<std::size_t>::max());
    EXPECT_FALSE(overflow.IsAllocated());
  }
  EXPECT_EQ(heap.allocated_bytes(), bytes);
  EXPECT_EQ(heap.committed_bytes(), committed);
}

TEST(NativeBufferTest, AllocationStatisticsIncludeReleasedNativeAndManagedStorage) {
  Isolate isolate;
  auto& heap = isolate.heap();
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  heap.ResetAllocationStatistics();
  std::size_t native_bytes = 0;
  {
    constexpr std::size_t kCount = 128;
    NativeBuffer<char> buffer(heap, kCount);
    ASSERT_TRUE(buffer.IsAllocated());
    native_bytes = heap.allocated_bytes() - baseline;
    EXPECT_EQ(heap.allocation_statistics().bytes, native_bytes);
    EXPECT_EQ(heap.allocation_statistics().count, 1U);
  }
  EXPECT_EQ(heap.allocated_bytes(), baseline);
  ASSERT_NE(heap.AllocateString("allocation statistics"), nullptr);
  const auto statistics = heap.allocation_statistics();
  EXPECT_GT(statistics.bytes, native_bytes);
  EXPECT_EQ(statistics.count, 2U);
  EXPECT_GE(statistics.peak_live_bytes, baseline + native_bytes);
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), baseline);
  EXPECT_EQ(heap.allocation_statistics().bytes, statistics.bytes);
  heap.ResetAllocationStatistics();
  const auto reset = heap.allocation_statistics();
  EXPECT_EQ(reset.bytes, 0U);
  EXPECT_EQ(reset.count, 0U);
  EXPECT_EQ(reset.peak_live_bytes, baseline);
}

}  // namespace
}  // namespace vela::runtime
