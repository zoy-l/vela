#include "benchmarks/heap_metrics.h"
#include <cstddef>

#include "benchmark/benchmark.h"

#include "runtime/heap.h"

namespace vela::benchmarks {

void CollectHeapBaseline(benchmark::State& state, runtime::Heap& heap) {
  heap.FinishCollection();
  heap.Collect();
  state.counters["baseline_bytes"] = static_cast<double>(heap.allocated_bytes());
}

std::size_t ReportHeap(benchmark::State& state, runtime::Heap& heap) {
  state.counters["allocated_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["committed_bytes"] = static_cast<double>(heap.committed_bytes());
  heap.FinishCollection();
  heap.Collect();
  const auto retained = heap.allocated_bytes();
  state.counters["retained_bytes"] = static_cast<double>(retained);
  return retained;
}

}  // namespace vela::benchmarks
