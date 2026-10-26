#pragma once

#include <cstdint>
#include <string_view>

namespace benchmark {
class State;
}

namespace vela::benchmarks {

void SteadyValue(benchmark::State& state, std::string_view source, std::int64_t expected);

}  // namespace vela::benchmarks
