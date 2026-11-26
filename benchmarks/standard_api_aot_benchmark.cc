#include <algorithm>
#include <array>
#include <string>

#include <benchmark/benchmark.h>

#include "benchmarks/aot_benchmark_memory.h"
#include "benchmarks/heap_metrics.h"
#include "benchmarks/standard_api_native/entries.h"
#include "benchmarks/standard_api_native/workloads.h"
#include "benchmarks/workload.h"
#include "benchmarks/workload_benchmark.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

namespace vela::benchmarks::standard_api {
namespace {

void Native(benchmark::State& state, Workload workload, WorkloadProblem problem) {
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
      !IsExpectedWorkloadResult(runtime::ExecuteNativeFunction(instance, *entry), problem)) {
    state.SkipWithError("AOT preparation or warm-up checksum failed");
    return;
  }
  auto& heap = isolate.heap();
  CollectHeapBaseline(state, heap);
  const auto& metrics = *std::ranges::find(kEntries, workload.name, &Entry::name);
  state.counters["entry_shadow_slots"] = static_cast<double>(metrics.shadow_slots);
  state.counters["entry_frame_bytes"] = static_cast<double>(metrics.frame_bytes);
  state.counters["entry_static_root_stores"] = static_cast<double>(metrics.root_stores);
  state.counters["suspended_frames"] =
      static_cast<double>(isolate.scheduler().suspended_frame_count());
  state.counters["saved_state_bytes"] =
      static_cast<double>(isolate.scheduler().saved_state_bytes());
  for (auto iteration : state) {
    auto result = runtime::ExecuteNativeFunction(instance, *entry);
    benchmark::DoNotOptimize(result);
    if (!IsExpectedWorkloadResult(result, problem)) {
      state.SkipWithError("AOT execution failed or checksum mismatched: " + result.message);
      return;
    }
  }
  ReportAotHeap(state, isolate);
  state.counters["result"] = static_cast<double>(problem.expected);
}

int Register() {
  for (const auto& workload : kStandardApiWorkloads) {
    for (const auto problem : workload.problems) {
      const auto suffix = std::string(workload.name) + "/" + std::to_string(problem.size);
      benchmark::RegisterBenchmark("aot/native/" + suffix, Native, workload, problem)
          ->UseRealTime();
      benchmark::RegisterBenchmark("aot/cpp/" + suffix, RunCppWorkload, workload, problem)
          ->UseRealTime();
    }
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = Register();

}  // namespace
}  // namespace vela::benchmarks::standard_api
