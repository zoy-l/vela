#pragma once

#include "benchmarks/production/contract.h"

namespace benchmark {
class State;
}

namespace vela::benchmarks::production {

void RunAot(benchmark::State& state, const Case& problem, const Options& options);

}  // namespace vela::benchmarks::production
