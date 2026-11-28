#include "benchmarks/production/measurement.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include <benchmark/benchmark.h>

#include "benchmarks/process_memory.h"
#include "benchmarks/production/allocation.h"

namespace vela::benchmarks::production {

double NearestRank(std::span<const double> sorted, std::size_t percentile) {
  constexpr std::size_t kPercent = 100;
  return sorted[(((sorted.size() * percentile) + kPercent - 1) / kPercent) - 1];
}

void LatencySamples::Report(benchmark::State& state) {
  if (recorded_ != samples_.size() || samples_.empty()) {
    state.SkipWithError("Incomplete latency sample buffer");
    return;
  }
  std::ranges::sort(samples_);
  state.counters["latency_samples"] = static_cast<double>(samples_.size());
  constexpr std::size_t kMedian = 50;
  constexpr std::size_t kTail95 = 95;
  constexpr std::size_t kTail99 = 99;
  state.counters["p50_ns"] = NearestRank(samples_, kMedian);
  state.counters["p95_ns"] = NearestRank(samples_, kTail95);
  state.counters["p99_ns"] = NearestRank(samples_, kTail99);
  state.counters["maximum_ns"] = samples_.back();
}

void ReportCppAllocations(benchmark::State& state,
                          const AllocationLedger& ledger,
                          std::uint64_t baseline) {
  const auto& values = ledger.statistics();
  if (!ledger.IsValid()) {
    state.SkipWithError("C++ allocation accounting underflow");
    return;
  }
  const auto count = static_cast<double>(state.iterations());
  state.counters["allocations_per_invocation"] = static_cast<double>(values.allocations) / count;
  state.counters["allocation_bytes_per_invocation"] = static_cast<double>(values.bytes) / count;
  state.counters["deallocations_per_invocation"] =
      static_cast<double>(values.deallocations) / count;
  state.counters["peak_live_growth_bytes"] = static_cast<double>(values.peak_bytes - baseline);
  state.counters["baseline_bytes"] = static_cast<double>(baseline);
  state.counters["retained_bytes"] = static_cast<double>(values.live_bytes);
}

void ReportResidentMemory(benchmark::State& state, std::uint64_t baseline) {
  const auto current = CurrentResidentBytes();
  const auto peak = PeakResidentBytes();
  if (!current || !peak) {
    state.SkipWithError(!current ? current.error() : peak.error());
    return;
  }
  state.counters["resident_baseline_bytes"] = static_cast<double>(baseline);
  state.counters["process_resident_bytes"] = static_cast<double>(*current);
  state.counters["process_peak_resident_bytes"] = static_cast<double>(*peak);
  state.counters["process_peak_growth_bytes"] =
      static_cast<double>(*peak > baseline ? *peak - baseline : 0);
}

}  // namespace vela::benchmarks::production
