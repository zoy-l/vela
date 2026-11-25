#include "benchmarks/workload_benchmark.h"

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

#include <benchmark/benchmark.h>

#include "benchmarks/aot_benchmark_memory.h"
#include "benchmarks/workload.h"
#include "runtime/native/native_execution.h"

namespace vela::benchmarks {

bool IsExpectedWorkloadResult(const runtime::ExecutionResult& result, WorkloadProblem problem) {
  return result.Succeeded() && result.value.IsInteger() &&
         result.value.AsInteger() == problem.expected;
}

void RunCppWorkload(benchmark::State& state, Workload workload, WorkloadProblem problem) {
  std::vector<std::int64_t> input(static_cast<std::size_t>(problem.size));
  std::ranges::iota(input, std::int64_t{0});
  if (workload.native(input) != problem.expected) {
    state.SkipWithError("C++ warm-up checksum mismatched");
    return;
  }
  for (auto iteration : state) {
    // 输入与结果屏障阻止编译器跨迭代合并整个负载。
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

}  // namespace vela::benchmarks
