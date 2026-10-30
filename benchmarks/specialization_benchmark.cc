#include <cstdint>
#include <string>

#include <benchmark/benchmark.h>

#include "compiler/frontend/ast_clone.h"
#include "compiler/frontend/checker.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/specialization.h"

namespace vela::benchmarks {
namespace {

enum class TemplateKind : std::uint8_t { Function, Class };

void SpecializeTemplates(benchmark::State& state, TemplateKind kind) {
  std::string body;
  const std::string value = kind == TemplateKind::Function ? "value" : "this.value";
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    body += "const local" + std::to_string(index) + ": T = " + value + ";";
  }
  body += "return " + value + ";";
  std::string source = kind == TemplateKind::Function
                           ? "func duplicate<T>(value: T): T {" + body + "}"
                           : "class Container<T> { value: T; read(): T {" + body + "} }";
  for (std::int64_t index = 0; index < state.range(1); ++index) {
    source += "class Item" + std::to_string(index) + " {}";
  }
  source += "func main(): void {";
  for (std::int64_t index = 0; index < state.range(1); ++index) {
    const auto name = "Item" + std::to_string(index);
    source += kind == TemplateKind::Function ? "duplicate<" : "Container<";
    source += name;
    source += kind == TemplateKind::Function ? ">(" : ">(value: ";
    source += name;
    source += kind == TemplateKind::Function ? "());" : "()).read();";
  }
  source += "}";
  const auto parsed = frontend::ParseSource(source);
  if (!parsed.diagnostics.empty()) {
    state.SkipWithError("Generic workload did not parse");
    return;
  }
  auto checked = frontend::CloneProgram(parsed.program, {});
  if (!frontend::CheckProgram(checked).Succeeded()) {
    state.SkipWithError("Generic workload did not type check");
    return;
  }
  for (auto iteration : state) {
    state.PauseTiming();
    auto program = frontend::CloneProgram(parsed.program, {});
    state.ResumeTiming();
    const auto diagnostics = frontend::SpecializeGenerics(program);
    if (!diagnostics.empty()) {
      state.SkipWithError("Generic workload did not specialize");
      break;
    }
    benchmark::DoNotOptimize(program);
  }
}

BENCHMARK_CAPTURE(SpecializeTemplates, Function, TemplateKind::Function)
    ->Name("specialize/function")
    ->Args({32, 4})
    ->Args({256, 16})
    ->Args({1024, 32});
BENCHMARK_CAPTURE(SpecializeTemplates, Class, TemplateKind::Class)
    ->Name("specialize/class")
    ->Args({32, 4})
    ->Args({256, 16})
    ->Args({1024, 32});

}  // namespace
}  // namespace vela::benchmarks
