#pragma once

namespace benchmark {
class State;
}

namespace vela::runtime {
class Isolate;
}

namespace vela::benchmarks {

void ReportAotResident(benchmark::State& state);
void ReportAotHeap(benchmark::State& state, runtime::Isolate& isolate);

}  // namespace vela::benchmarks
