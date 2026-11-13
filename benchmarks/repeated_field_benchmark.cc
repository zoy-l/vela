#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

#include "compiler/aot/native_compiler.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/ir/preparation.h"

namespace vela::benchmarks {
namespace {

void RepeatedFieldCompilation(benchmark::State& state, std::string_view bound) {
  std::string source =
      "class Point { x: double; dx: double; } let points: Point[] = []; "
      "export func update(count: int): void {";
  for (std::int64_t traversal = 0; traversal < state.range(0); ++traversal) {
    source += "for (let round = 0; round < " + std::string(bound) +
              "; round++) { for (const point in points) { point.x += point.dx; } }";
  }
  source += '}';
  const frontend::SourceInput input{.path = "repeated_fields.vela", .text = source};
  const auto prepared = ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
  if (!prepared.Succeeded()) {
    state.SkipWithError(frontend::DiagnosticsText(prepared.diagnostics));
    return;
  }
  std::chrono::nanoseconds optimization{};
  for (auto iteration : state) {
    auto compiled = compiler::aot::Compile(prepared.program, {.exports = {"update"}});
    if (!compiled) {
      state.SkipWithError(frontend::DiagnosticsText(compiled.error()));
      return;
    }
    if (compiled->object.empty() || compiled->exports.size() != 1) {
      state.SkipWithError("Repeated-field compilation did not emit the requested artifact");
      return;
    }
    optimization += compiled->timings.optimization;
    benchmark::DoNotOptimize(compiled);
  }
  state.counters["optimization_ns"] = benchmark::Counter(static_cast<double>(optimization.count()),
                                                         benchmark::Counter::kAvgIterations);
}

BENCHMARK_CAPTURE(RepeatedFieldCompilation, Dynamic, "count")
    ->Name("compile/aot/repeated_fields/dynamic")
    ->Arg(8)
    ->UseRealTime();
BENCHMARK_CAPTURE(RepeatedFieldCompilation, OverLimit, "64")
    ->Name("compile/aot/repeated_fields/over_limit")
    ->Arg(8)
    ->UseRealTime();
BENCHMARK_CAPTURE(RepeatedFieldCompilation, Foldable, "10")
    ->Name("compile/aot/repeated_fields/foldable")
    ->Arg(1)
    ->Arg(8)
    ->UseRealTime();

}  // namespace
}  // namespace vela::benchmarks
