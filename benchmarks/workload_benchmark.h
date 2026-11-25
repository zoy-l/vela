#pragma once

#include "benchmarks/workload.h"

namespace benchmark {
class State;
}
namespace vela::runtime {
struct ExecutionResult;
}

namespace vela::benchmarks {

bool IsExpectedWorkloadResult(const runtime::ExecutionResult& result, WorkloadProblem problem);
void RunCppWorkload(benchmark::State& state, Workload workload, WorkloadProblem problem);

}  // namespace vela::benchmarks
