#include "benchmarks/production/measurement.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "benchmarks/production/allocation.h"
#include "benchmarks/production/async_runtime.h"
#include "benchmarks/production/contract.h"

namespace vela::benchmarks::production {

TEST(ProductionMeasurementTest, NearestRankIncludesSmallSamplesAndEndpoints) {
  const std::array values{1.0, 2.0, 3.0, 4.0};
  EXPECT_EQ(NearestRank(values, 50), 2);
  EXPECT_EQ(NearestRank(values, 95), 4);
  EXPECT_EQ(NearestRank(values, 99), 4);
  EXPECT_EQ(NearestRank(values, 100), 4);
  const std::array single{7.0};
  EXPECT_EQ(NearestRank(single, 99), 7);
}

TEST(ProductionMeasurementTest, CountsGrowthReallocationAndCleanupWithoutResettingLiveBytes) {
  AllocationLedger ledger;
  const AllocationScope scope(ledger);
  constexpr std::size_t kInitialBytes = 17;
  constexpr std::size_t kResizedBytes = 29;
  auto* allocation = AllocateCounted(&ledger, kInitialBytes);
  ledger.ResetActivity();
  EXPECT_EQ(ledger.statistics().live_bytes, 17);
  EXPECT_EQ(ledger.statistics().allocations, 0);
  allocation = ReallocateCounted(&ledger, allocation, kInitialBytes, kResizedBytes);
  EXPECT_EQ(ledger.statistics().live_bytes, 29);
  EXPECT_EQ(ledger.statistics().peak_bytes, 46);
  FreeCounted(&ledger, allocation);
  EXPECT_EQ(ledger.statistics().live_bytes, 0);
  EXPECT_TRUE(ledger.IsValid());
}

static Task<true> TraceTask(Scheduler<true>* scheduler,
                            std::vector<std::int64_t>* trace,
                            std::int64_t identity) {
  trace->push_back(identity);
  co_await Scheduler<true>::Yield{.scheduler = scheduler};
  constexpr std::int64_t kResumeOffset = 10;
  trace->push_back(identity + kResumeOffset);
  co_return identity;
}

static Task<true> ContinueTask([[maybe_unused]] Scheduler<true>* scheduler,
                               Task<true>* input,
                               std::vector<std::int64_t>* trace) {
  const auto result = co_await *input;
  constexpr std::int64_t kContinuationMarker = 99;
  trace->push_back(kContinuationMarker);
  co_return result;
}

TEST(ProductionSchedulerTest, EagerStartFifoResumeContinuationAndFrameCleanup) {
  AllocationLedger ledger;
  const AllocationScope scope(ledger);
  std::vector<std::int64_t> trace;
  {
    Scheduler<true> scheduler;
    const auto first = TraceTask(&scheduler, &trace, 1);
    auto second = TraceTask(&scheduler, &trace, 2);
    const auto continuation = ContinueTask(&scheduler, &second, &trace);
    EXPECT_EQ(trace, (std::vector<std::int64_t>{1, 2}));
    EXPECT_FALSE(first.IsDone());
    scheduler.Drain();
    EXPECT_EQ(trace, (std::vector<std::int64_t>{1, 2, 11, 12, 99}));
    EXPECT_TRUE(first.IsDone());
    EXPECT_TRUE(continuation.IsDone());
    EXPECT_EQ(continuation.value(), 2);
  }
  EXPECT_EQ(ledger.statistics().live_bytes, 0);
  EXPECT_EQ(ledger.statistics().allocations, ledger.statistics().deallocations);
}

static Task<true> EventTask([[maybe_unused]] Scheduler<true>* scheduler, Event<true>* event) {
  co_await *event;
  co_return kSuccess;
}

static Task<true> DelayedTask(Scheduler<true>* scheduler,
                              std::vector<std::int64_t>* trace,
                              std::int64_t identity) {
  co_await Scheduler<true>::Delay{.scheduler = scheduler, .duration = {}};
  trace->push_back(identity);
  co_return identity;
}

TEST(ProductionSchedulerTest, EventFanoutCompletedAwaitAndTimerOrder) {
  AllocationLedger ledger;
  const AllocationScope scope(ledger);
  std::vector<std::int64_t> trace;
  {
    Scheduler<true> scheduler;
    Event<true> event;
    const auto first = EventTask(&scheduler, &event);
    event.Signal(&scheduler);
    const auto second = EventTask(&scheduler, &event);
    const auto timer1 = DelayedTask(&scheduler, &trace, 1);
    const auto timer2 = DelayedTask(&scheduler, &trace, 2);
    EXPECT_TRUE(second.IsDone());
    scheduler.Drain();
    EXPECT_EQ(first.value(), 42);
    EXPECT_EQ(trace, (std::vector<std::int64_t>{1, 2}));
    EXPECT_TRUE(timer1.IsDone());
    EXPECT_TRUE(timer2.IsDone());
  }
  EXPECT_TRUE(ledger.IsValid());
  EXPECT_EQ(ledger.statistics().live_bytes, 0);
}

}  // namespace vela::benchmarks::production
