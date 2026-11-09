#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ratio>
#include <span>
#include <vector>

#include "benchmark/benchmark.h"

#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::benchmarks {
namespace {

constexpr std::size_t kYoungObjects = 4096;
constexpr std::int64_t kRetainedGraph = 8192;
constexpr std::int64_t kSmallSurvivorSet = 64;
constexpr std::int64_t kSmallStepBudget = 16;
constexpr std::int64_t kLargeStepBudget = 64;
constexpr std::size_t kStepPercentile = 95;
constexpr std::size_t kPercent = 100;

void CollectionCycle(benchmark::State& state) {
  using Clock = std::chrono::steady_clock;
  const auto old_count = static_cast<std::size_t>(state.range(0));
  const auto survivor_count = static_cast<std::size_t>(state.range(1));
  const auto budget = static_cast<std::size_t>(state.range(2));
  double maximum_step_ns = 0;
  double percentile_step_ns = 0;
  double start_ns = 0;
  double committed_bytes = 0;
  double live_bytes = 0;
  double before_bytes = 0;
  double reclaimed_bytes = 0;
  double reclaimed_objects = 0;
  double step_count = 0;
  for (auto iteration : state) {
    runtime::Isolate isolate(std::numeric_limits<std::size_t>::max());
    auto& heap = isolate.heap();
    const auto references = std::to_array<std::uint32_t>({0, 1});
    auto* layout = isolate.CreateClassLayout(2, references);
    runtime::Value head;
    runtime::RootRange head_root(heap, &head, 1);
    std::vector<runtime::HeapObject*> old_objects;
    old_objects.reserve(old_count);
    for (std::size_t index = 0; index < old_count; ++index) {
      const std::array fields{head, runtime::Value::Null()};
      auto* object = heap.AllocateInstance(*layout, fields);
      if (object == nullptr) {
        state.SkipWithError("cannot allocate retained graph");
        return;
      }
      old_objects.push_back(object);
      head = runtime::Value::Object(object);
    }
    heap.Collect();
    std::vector<runtime::Value> survivors(survivor_count);
    runtime::RootRange survivor_roots(heap, survivors.data(), survivors.size());
    for (std::size_t index = 0; index < kYoungObjects; ++index) {
      auto* string = heap.AllocateString("temporary script text");
      if (string == nullptr) {
        state.SkipWithError("cannot allocate young cohort");
        return;
      }
      if (index < survivor_count) {
        const auto value = runtime::Value::String(string);
        survivors.at(index) = value;
        if (index < old_count && !heap.SetField(*old_objects.at(index), 1, value)) {
          state.SkipWithError("cannot update retained object");
          return;
        }
      }
    }
    std::vector<double> steps;
    steps.reserve(((old_count + kYoungObjects) * 2 / budget) + 2);
    const auto allocated_bytes = heap.allocated_bytes();
    const auto allocated_objects = heap.object_count();
    const auto start = Clock::now();
    heap.StartCollection();
    start_ns += std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    while (heap.IsCollecting()) {
      const auto step_start = Clock::now();
      heap.Step(budget);
      const auto elapsed =
          std::chrono::duration<double, std::nano>(Clock::now() - step_start).count();
      maximum_step_ns = std::max(maximum_step_ns, elapsed);
      steps.push_back(elapsed);
    }
    state.SetIterationTime(std::chrono::duration<double>(Clock::now() - start).count());
    std::ranges::sort(steps);
    const auto steps_in_cycle = steps.size();
    percentile_step_ns += steps.at((steps_in_cycle - 1) * kStepPercentile / kPercent);
    step_count += static_cast<double>(steps_in_cycle);
    const auto remaining_objects = heap.object_count();
    const auto remaining_bytes = heap.allocated_bytes();
    if (remaining_objects != old_count + survivor_count) {
      state.SkipWithError("collector retained the wrong cohort");
      return;
    }
    committed_bytes = static_cast<double>(heap.committed_bytes());
    live_bytes = static_cast<double>(remaining_bytes);
    before_bytes = static_cast<double>(allocated_bytes);
    reclaimed_bytes = static_cast<double>(allocated_bytes - remaining_bytes);
    reclaimed_objects = static_cast<double>(allocated_objects - remaining_objects);
  }
  state.counters["old_objects"] = static_cast<double>(old_count);
  state.counters["new_objects"] = static_cast<double>(kYoungObjects);
  state.counters["new_survivors"] = static_cast<double>(survivor_count);
  state.counters["maximum_step_ns"] = maximum_step_ns;
  const auto iterations = static_cast<double>(state.iterations());
  state.counters["p95_step_ns"] = percentile_step_ns / iterations;
  state.counters["start_ns"] = start_ns / iterations;
  state.counters["steps_per_cycle"] = step_count / iterations;
  state.counters["heap_before_bytes"] = before_bytes;
  state.counters["reclaimed_bytes"] = reclaimed_bytes;
  state.counters["reclaimed_objects"] = reclaimed_objects;
  state.counters["heap_live_bytes"] = live_bytes;
  state.counters["heap_committed_bytes"] = committed_bytes;
}

BENCHMARK(CollectionCycle)
    ->Name("gc/cohort")
    ->Args({0, kSmallSurvivorSet, kSmallStepBudget})
    ->Args({kRetainedGraph, kSmallSurvivorSet, kSmallStepBudget})
    ->Args({kRetainedGraph, static_cast<std::int64_t>(kYoungObjects), kSmallStepBudget})
    ->Args({kRetainedGraph, kSmallSurvivorSet, kLargeStepBudget})
    ->UseManualTime();

}  // namespace
}  // namespace vela::benchmarks
