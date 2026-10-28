#include "benchmarks/aot_benchmark_memory.h"

#include <benchmark/benchmark.h>

#include "benchmarks/process_memory.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"

namespace vela::benchmarks {

void ReportAotResident(benchmark::State& state) {
  const auto resident = CurrentResidentBytes();
  if (!resident) {
    state.SkipWithError(resident.error());
  } else {
    state.counters["process_resident_bytes"] = static_cast<double>(*resident);
  }
}

void ReportAotHeap(benchmark::State& state, runtime::Isolate& isolate) {
  auto& heap = isolate.heap();
  auto& scheduler = isolate.scheduler();
  state.counters["allocated_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["committed_bytes"] = static_cast<double>(heap.committed_bytes());
  heap.FinishCollection();
  heap.Collect();
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes());
  const auto suspended_frames = scheduler.suspended_frame_count();
  const auto saved_bytes = scheduler.saved_state_bytes();
  state.counters["remaining_suspended_frames"] = static_cast<double>(suspended_frames);
  state.counters["remaining_saved_bytes"] = static_cast<double>(saved_bytes);
  if (state.counters["baseline_bytes"] != state.counters["retained_bytes"] ||
      suspended_frames != 0 || saved_bytes != 0) {
    state.SkipWithError("benchmark retained temporary state after completion and GC");
  }
  ReportAotResident(state);
}

}  // namespace vela::benchmarks
