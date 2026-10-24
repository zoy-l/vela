#include <cstdint>
#include <string>

#include "benchmark/benchmark.h"

#include "compiler/compiler.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::benchmarks {
namespace {

auto BuildSymbolSource(std::int64_t count) -> std::string {
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
    const auto* entry = compiled.program.FindFunction("main");
    if (entry == nullptr) {
      state.SkipWithError("Symbol workload has no main function");
      return;
    }
    const auto entry_index = static_cast<std::uint32_t>(entry - compiled.program.functions.data());
    const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.View(), entry_index);
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
}

BENCHMARK(CompileSymbolTables)->Name("compile/symbol_tables")->Arg(32)->Arg(256)->Arg(1024);

}  // namespace
}  // namespace vela::benchmarks
