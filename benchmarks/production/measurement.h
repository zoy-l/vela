#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ratio>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

#include "benchmarks/process_memory.h"
#include "benchmarks/production/allocation.h"
#include "benchmarks/production/contract.h"

namespace vela::benchmarks::production {

double NearestRank(std::span<const double> sorted, std::size_t percentile);

class LatencySamples final {
 public:
  using Clock = std::chrono::steady_clock;
  explicit LatencySamples(std::uint64_t count) { samples_.resize(static_cast<std::size_t>(count)); }
  void Record(benchmark::State& state, Clock::time_point started) {
    const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - started).count();
    samples_[recorded_++] = elapsed;
    state.SetIterationTime(elapsed / static_cast<double>(std::nano::den));
  }
  void Report(benchmark::State& state);

 private:
  std::vector<double> samples_;
  std::size_t recorded_ = 0;
};

void ReportCppAllocations(benchmark::State& state,
                          const AllocationLedger& ledger,
                          std::uint64_t baseline);
void ReportResidentMemory(benchmark::State& state, std::uint64_t baseline);

template <class Reference>
void FinishReferenceAllocations(benchmark::State& state,
                                const Case& problem,
                                Reference& reference,
                                std::uint64_t baseline) {
  auto& ledger = *ActiveAllocationLedger();
  ReportCppAllocations(state, ledger, baseline);
  if (problem.family == Family::Churn) {
    const auto steady = ledger.statistics().live_bytes;
    state.counters["steady_live_bytes"] = static_cast<double>(steady);
    if (steady != baseline) {
      state.SkipWithError("C++ retained ring did not reach a stable memory plateau");
    }
    if constexpr (requires { reference.Cleanup(); }) {
      reference.Cleanup();
    }
    state.counters["retained_bytes"] = static_cast<double>(ledger.statistics().live_bytes);
    state.counters["expected_retained_bytes"] = 0;
    if (ledger.statistics().live_bytes != 0) {
      state.SkipWithError("C++ churn cleanup retained storage");
    }
  } else {
    state.counters["expected_retained_bytes"] = static_cast<double>(baseline);
    if (ledger.statistics().live_bytes != baseline) {
      state.SkipWithError("C++ workload retained temporary storage");
    }
  }
}

template <Measurement Mode, class Reference>
void RunReference(benchmark::State& state,
                  const Case& problem,
                  Reference& reference,
                  const Options& options) {
  const auto warmups = problem.family == Family::Churn ? kRingCapacity : std::size_t{1};
  for (std::size_t warmup = 0; warmup < warmups; ++warmup) {
    if (reference.Run() != problem.expected) {
      state.SkipWithError("C++ warm-up checksum mismatch");
      return;
    }
  }
  LatencySamples samples(Mode == Measurement::Latency ? options.samples : 0);
  std::uint64_t baseline = 0;
  std::uint64_t resident = 0;
  if constexpr (Mode == Measurement::Allocations) {
    auto& ledger = *ActiveAllocationLedger();
    ledger.ResetActivity();
    baseline = ledger.statistics().live_bytes;
  }
  if constexpr (Mode == Measurement::Latency) {
    const auto current = CurrentResidentBytes();
    if (!current) {
      state.SkipWithError(current.error());
      return;
    }
    resident = *current;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    benchmark::ClobberMemory();
    [[maybe_unused]] LatencySamples::Clock::time_point started;
    if constexpr (Mode == Measurement::Latency) {
      started = LatencySamples::Clock::now();
    }
    auto value = reference.Run();
    benchmark::DoNotOptimize(value);
    if (value != problem.expected) {
      state.SkipWithError("C++ checksum mismatch");
      return;
    }
    if constexpr (Mode == Measurement::Latency) {
      samples.Record(state, started);
    }
  }
  if constexpr (Mode == Measurement::Latency) {
    samples.Report(state);
    ReportResidentMemory(state, resident);
  }
  if constexpr (Mode == Measurement::Allocations) {
    FinishReferenceAllocations(state, problem, reference, baseline);
  }
  state.counters["result"] = static_cast<double>(problem.expected);
  state.counters["expected_result"] = static_cast<double>(problem.expected);
}

}  // namespace vela::benchmarks::production
