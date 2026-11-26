#include <cstdint>
#include <span>
#include <string>

#include <benchmark/benchmark.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/ir/preparation.h"

namespace vela::benchmarks {
namespace {

void PrepareContracts(benchmark::State& state, const std::string& source) {
  const frontend::SourceInput input{.path = "contracts.vela", .text = source};
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
    if (!result.Succeeded() || result.program.functions.empty()) {
      state.SkipWithError(frontend::DiagnosticsText(result.diagnostics));
      return;
    }
    benchmark::DoNotOptimize(result.program);
  }
}

void PrepareInheritedConstructors(benchmark::State& state) {
  std::string source;
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    const auto suffix = std::to_string(index);
    source.append("class Base")
        .append(suffix)
        .append(" { value: int; extra: int = 2; } export class Child")
        .append(suffix)
        .append(" extends Base")
        .append(suffix)
        .append(" { constructor(super.value); }");
  }
  PrepareContracts(state, source);
}

void PreparePredicates(benchmark::State& state) {
  std::string source =
      "class Base { value: int = 42; } class First extends Base {} class Second extends Base {}";
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    const auto suffix = std::to_string(index);
    source.append("func guard")
        .append(suffix)
        .append(
            "(value: dynamic, flag: bool): bool { if (flag && value is First) { return true; }"
            "if (value is Second) { return true; } if (value is First) { return true; }"
            "if (value is Second) { return true; } return false; }");
    source.append("export func select")
        .append(suffix)
        .append("(values: dynamic[]): Base[] { return values.filter(value => guard")
        .append(suffix)
        .append("(value, true)); }");
  }
  PrepareContracts(state, source);
}

BENCHMARK(PrepareInheritedConstructors)
    ->Name("compiler/contracts/inherited_constructors")
    ->Arg(64)
    ->UseRealTime();
BENCHMARK(PreparePredicates)->Name("compiler/contracts/predicates")->Arg(32)->UseRealTime();

}  // namespace
}  // namespace vela::benchmarks
