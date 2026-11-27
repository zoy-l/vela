#include <cstdint>
#include <string>

#include "benchmark/benchmark.h"

#include "benchmarks/standard_value_benchmark.h"

namespace vela::benchmarks {
namespace {

constexpr std::int64_t kExpectedResult = 42;

void FillInlineStructs(benchmark::State& state) {
  const auto count = std::to_string(state.range(0));
  const std::string source =
      "struct Pair { first: int; second: int; } "
      "const values: Pair[] = make(); func make(): Pair[] { "
      "const result: Pair[] = []; result.reserve(" +
      count + "); for (let i = 0; i < " + count +
      "; i++) { result.push(Pair(first: 0, second: 0)); } return result; } "
      "const replacement = Pair(first: 40, second: 2); "
      "func main(): int { values.fill(replacement); "
      "const value = values[values.length - 1]; return value.first + value.second; }";
  SteadyValue(state, source, kExpectedResult, HeapBaseline::AfterWarmup);
}

BENCHMARK(FillInlineStructs)->Name("array_fill/structs")->Arg(4096)->UseRealTime();

}  // namespace
}  // namespace vela::benchmarks
