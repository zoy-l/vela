#include <cstdint>
#include <string>

#include "benchmark/benchmark.h"

#include "compiler/compiler.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/token.h"
#include "compiler/frontend/type_checker.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::benchmarks {
namespace {

std::string BuildSymbolSource(std::int64_t count) {
  std::string source;
  for (std::int64_t index = 0; index < count; ++index) {
    const std::string suffix = std::to_string(index);
    const std::string class_name = "SymbolTableBenchmarkClass" + suffix;
    source += "enum SymbolTableBenchmarkEnum" + suffix + " { first, second }\n";
    source += "class " + class_name +
              " { value: int; "
              "increment(amount: int): int { return this.value + amount; } }\n";
    source += "func symbol_table_benchmark_function_" + suffix +
              "(parameter_with_a_long_name: int): int { "
              "let local_with_a_long_name: int = parameter_with_a_long_name; "
              "return local_with_a_long_name + 1; }\n";
  }
  // 只调用首个函数，使符号规模不受当前 Call 指令的 8 位函数编号限制。
  source += "func main(): int { return symbol_table_benchmark_function_0(10); }\n";
  return source;
}

void CompileSymbolTables(benchmark::State& state) {
  const std::string source = BuildSymbolSource(state.range(0));
  // 在计时外验证生成的负载；测量区间只包括编译及其资源生命周期。
  {
    runtime::Isolate isolate;
    const auto compiled = compiler::CompileSource(source, isolate);
    if (!compiled.Succeeded()) {
      state.SkipWithError("Symbol workload did not compile");
      return;
    }
    const auto entry = compiled.program.FindFunction("main");
    if (!entry.IsValid()) {
      state.SkipWithError("Symbol workload has no main function");
      return;
    }
    const auto entry_index = entry.index();
    const auto result = vm::VirtualMachine(isolate).Execute(compiled.program, entry_index);
    constexpr std::int64_t kExpectedResult = 11;
    if (!result.Succeeded() || !result.value.IsInteger() ||
        result.value.AsInteger() != kExpectedResult) {
      state.SkipWithError("Symbol workload returned an unexpected result");
      return;
    }
  }
  for (auto iteration : state) {
    runtime::Isolate isolate;
    auto compiled = compiler::CompileSource(source, isolate);
    if (!compiled.Succeeded()) {
      state.SkipWithError("Symbol workload did not compile");
      break;
    }
    benchmark::DoNotOptimize(compiled);
  }
  state.counters["source_bytes"] = static_cast<double>(source.size());
  state.counters["token_bytes"] = static_cast<double>(sizeof(frontend::Token));
}

BENCHMARK(CompileSymbolTables)->Name("compile/symbol_tables")->Arg(32)->Arg(256)->Arg(1024);

void ParseComparisons(benchmark::State& state) {
  std::string source = "func compare(a: int, b: int): bool { return ";
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    source += index == 0 ? "a < b" : " && a < b";
  }
  source += "; }";
  for (auto iteration : state) {
    auto parsed = frontend::ParseSource(source);
    if (!parsed.diagnostics.empty()) {
      state.SkipWithError("Comparison workload did not parse");
      break;
    }
    benchmark::DoNotOptimize(parsed);
  }
}

BENCHMARK(ParseComparisons)->Name("parse/comparisons")->Arg(128)->Arg(512)->Arg(2048);

void CheckRefinements(benchmark::State& state) {
  std::string source = "func choose(flag: bool";
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    source += ", value" + std::to_string(index) + ": string?";
  }
  source += "): int {";
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    source += "if (value" + std::to_string(index) + " == null) return 0;";
  }
  source += "let result = 0;";
  for (std::int64_t branch = 0; branch < state.range(0); ++branch) {
    source += "if (flag) { result += ";
    for (std::int64_t index = 0; index < state.range(0); ++index) {
      source += (index == 0 ? "value" : " + value") + std::to_string(index) + ".length";
    }
    source += "; } else { result += 1; }";
  }
  source += "return result; }";
  const auto parsed = frontend::ParseSource(source);
  if (!parsed.diagnostics.empty()) {
    state.SkipWithError("Refinement workload did not parse");
    return;
  }
  for (auto iteration : state) {
    auto checked = frontend::TypeChecker().Check(parsed.program);
    if (!checked.diagnostics.empty()) {
      state.SkipWithError("Refinement workload did not type check");
      break;
    }
    benchmark::DoNotOptimize(checked);
  }
}

BENCHMARK(CheckRefinements)->Name("check/refinements")->Arg(8)->Arg(32)->Arg(128);

enum class BranchKind { Match, Switch };

void CheckBranchRefinements(benchmark::State& state, BranchKind kind) {
  std::string source = "func choose(selector: int";
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    source += ", value" + std::to_string(index) + ": string?";
  }
  source += "): int {";
  std::string sum;
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    source += "if (value" + std::to_string(index) + " == null) return 0;";
    sum += (index == 0 ? "value" : " + value") + std::to_string(index) + ".length";
  }
  source += kind == BranchKind::Match ? "return match (selector) {" : "switch (selector) {";
  for (std::int64_t arm = 0; arm < state.range(1); ++arm) {
    source += kind == BranchKind::Match ? std::to_string(arm) + " => " + sum + ","
                                        : "case " + std::to_string(arm) + ": return " + sum + ";";
  }
  source +=
      kind == BranchKind::Match ? "_ => " + sum + ", }; }" : "default: return " + sum + "; } }";
  const auto parsed = frontend::ParseSource(source);
  if (!parsed.diagnostics.empty()) {
    state.SkipWithError("Branch refinement workload did not parse");
    return;
  }
  for (auto iteration : state) {
    auto checked = frontend::TypeChecker().Check(parsed.program);
    if (!checked.diagnostics.empty()) {
      state.SkipWithError("Branch refinement workload did not type check");
      break;
    }
    benchmark::DoNotOptimize(checked);
  }
}

BENCHMARK_CAPTURE(CheckBranchRefinements, Match, BranchKind::Match)
    ->Name("check/branch_refinements/match")
    ->Args({8, 16})
    ->Args({64, 32})
    ->Args({128, 64});
BENCHMARK_CAPTURE(CheckBranchRefinements, Switch, BranchKind::Switch)
    ->Name("check/branch_refinements/switch")
    ->Args({8, 16})
    ->Args({64, 32})
    ->Args({128, 64});

}  // namespace
}  // namespace vela::benchmarks
