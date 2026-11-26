#include <cstdint>
#include <fstream>
#include <string>

#include <benchmark/benchmark.h>

#include "base/path.h"
#include "benchmarks/temporary_directory.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/tooling/documentation/documentation.h"

namespace vela::benchmarks {
namespace {

void DocumentSource(benchmark::State& state, const std::string& source, const char* expected) {
  const BenchmarkDirectory temporary(state);
  if (!temporary.IsValid()) {
    return;
  }
  const auto path = temporary.path() / "module.vela";
  {
    std::ofstream output(path);
    output << source;
    if (!output) {
      state.SkipWithError("could not write documentation source");
      return;
    }
  }
  const auto entry = PathText(path);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = tooling::GenerateDocumentation(entry);
    if (!result.Succeeded() || !result.markdown.contains(expected)) {
      state.SkipWithError(frontend::DiagnosticsText(result.diagnostics) +
                          "missing constructor documentation");
      return;
    }
    benchmark::DoNotOptimize(result.markdown);
  }
}

void DocumentFunctionBodies(benchmark::State& state) {
  std::string source = "export class Counter { value: int = 0; increment(): int {";
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    source += "this.value += 1;";
  }
  source += "return this.value; } }";
  DocumentSource(state, source, "Counter(value: int = …);");
}

void DocumentGenericClasses(benchmark::State& state) {
  std::string source;
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    source += "export class Pair" + std::to_string(index) +
              "<T, U> { first: T; second: U; constructor(this.first, this.second); }";
  }
  DocumentSource(state, source, "Pair0(first: T, second: U);");
}

BENCHMARK(DocumentFunctionBodies)
    ->Name("tooling/documentation/function_bodies")
    ->Arg(64)
    ->Arg(4096)
    ->UseRealTime();

BENCHMARK(DocumentGenericClasses)
    ->Name("tooling/documentation/generic_classes")
    ->Arg(32)
    ->Arg(128)
    ->UseRealTime();

}  // namespace
}  // namespace vela::benchmarks
