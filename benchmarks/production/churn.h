#pragma once

#include <benchmark/benchmark.h>

#include "benchmarks/production/contract.h"

namespace vela::benchmarks::production {

void RunChurn(benchmark::State& state, const Case& problem, const Options& options);

}  // namespace vela::benchmarks::production
