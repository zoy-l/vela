#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "benchmarks/aot_benchmark_memory.h"
#include "benchmarks/heap_metrics.h"
#include "benchmarks/standard_api_native/entries.h"
#include "benchmarks/standard_api_workloads.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_instance.h"

namespace vela::benchmarks::standard_api {
namespace {

bool IsExpected(const runtime::ExecutionResult& result, WorkloadProblem problem) {
  return result.Succeeded() && result.value.IsInteger() &&
         result.value.AsInteger() == problem.expected;
}

void Native(benchmark::State& state, StandardApiWorkload workload, WorkloadProblem problem) {
  runtime::Isolate isolate;
  const auto loaded = runtime::NativeInstance::Instantiate(isolate, *kImage);
  if (!loaded) {
    state.SkipWithError(loaded.error().message);
    return;
  }
  auto& instance = **loaded;
  const auto setup = instance.FindFunction("prepare");
  const auto entry = instance.FindFunction(workload.name);
  if (!setup || !entry) {
    state.SkipWithError("Missing AOT standard API entry");
    return;
  }
  const auto arguments = std::to_array({runtime::Value::Integer(problem.size)});
  if (!runtime::ExecuteNativeFunction(instance, *setup, arguments).Succeeded() ||
      !IsExpected(runtime::ExecuteNativeFunction(instance, *entry), problem)) {
    state.SkipWithError("AOT preparation or warm-up checksum failed");
    return;
  }
  auto& heap = isolate.heap();
  CollectHeapBaseline(state, heap);
  const auto metrics = std::ranges::find(kEntries, workload.name, &Entry::name);
  state.counters["entry_shadow_slots"] = static_cast<double>(metrics->shadow_slots);
  state.counters["entry_frame_bytes"] = static_cast<double>(metrics->frame_bytes);
  state.counters["entry_static_root_stores"] = static_cast<double>(metrics->root_stores);
  state.counters["suspended_frames"] =
      static_cast<double>(isolate.scheduler().suspended_frame_count());
  state.counters["saved_state_bytes"] =
      static_cast<double>(isolate.scheduler().saved_state_bytes());
  for (auto iteration : state) {
    auto result = runtime::ExecuteNativeFunction(instance, *entry);
    benchmark::DoNotOptimize(result);
    if (!IsExpected(result, problem)) {
      state.SkipWithError("AOT execution failed or checksum mismatched: " + result.message);
      return;
    }
  }
  ReportAotHeap(state, isolate);
  state.counters["result"] = static_cast<double>(problem.expected);
}

void Cpp(benchmark::State& state, StandardApiWorkload workload, WorkloadProblem problem) {
  std::vector<std::int64_t> input(static_cast<std::size_t>(problem.size));
  std::ranges::iota(input, std::int64_t{0});
  if (workload.native(input) != problem.expected) {
    state.SkipWithError("C++ warm-up checksum mismatched");
    return;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(input.data());
    auto result = workload.native(input);
    benchmark::DoNotOptimize(result);
    if (result != problem.expected) {
      state.SkipWithError("C++ checksum mismatched");
      return;
    }
  }
  state.counters["result"] = static_cast<double>(problem.expected);
  ReportAotResident(state);
}

int Register() {
  for (const auto& workload : kStandardApiWorkloads) {
    for (const auto problem : workload.problems) {
      const auto suffix = std::string(workload.name) + "/" + std::to_string(problem.size);
      benchmark::RegisterBenchmark("aot/native/" + suffix, Native, workload, problem)
          ->UseRealTime();
      benchmark::RegisterBenchmark("aot/cpp/" + suffix, Cpp, workload, problem)->UseRealTime();
    }
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = Register();

}  // namespace
}  // namespace vela::benchmarks::standard_api
