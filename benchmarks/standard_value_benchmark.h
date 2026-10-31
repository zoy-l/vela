#pragma once

#include <cstdint>
#include <string_view>

namespace benchmark {
class State;
}

namespace vela::benchmarks {

enum class HeapBaseline : std::uint8_t { BeforeWarmup, AfterWarmup };

void SteadyValue(benchmark::State& state,
                 std::string_view source,
                 std::int64_t expected,
                 HeapBaseline baseline = HeapBaseline::BeforeWarmup);

}  // namespace vela::benchmarks
