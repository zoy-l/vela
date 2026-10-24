#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "benchmark/benchmark.h"

#include "compiler/compiler.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::benchmarks {
namespace {

auto LoadSource(benchmark::State& state, std::string_view filename) -> std::optional<std::string> {
  const auto path = std::filesystem::path(VELA_BENCHMARK_SOURCE_DIR) / filename;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    state.SkipWithError("Cannot open benchmark source: " + path.string());
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void ReportCompileFailure(benchmark::State& state, const compiler::CompileResult& compiled) {
  std::string message;
  for (const auto& diagnostic : compiled.diagnostics) {
    message += std::to_string(diagnostic.location.line) + ":" +
               std::to_string(diagnostic.location.column) + ": " + diagnostic.message + "\n";
  }
  state.SkipWithError(message);
}

auto IsExpectedResult(const vm::ExecutionResult& result, std::int64_t expected) -> bool {
  return result.Succeeded() && result.value.IsInteger() && result.value.AsInteger() == expected;
}

void Compile(benchmark::State& state, std::string_view filename) {
  const auto source = LoadSource(state, filename);
  if (!source) {
    return;
  }
  for (auto iteration : state) {
    runtime::Isolate isolate;
    auto compiled = compiler::CompileSource(*source, isolate);
    if (!compiled.Succeeded()) {
      ReportCompileFailure(state, compiled);
      break;
    }
    benchmark::DoNotOptimize(compiled);
  }
}

void FirstExecution(benchmark::State& state, std::string_view filename, std::int64_t expected) {
  const auto source = LoadSource(state, filename);
  if (!source) {
    return;
  }
  for (auto iteration : state) {
    state.PauseTiming();
    {
      // 每次使用新的 Isolate 和字节码；编译、验证及销毁不计入首次执行时间。
      runtime::Isolate isolate;
      const auto compiled = compiler::CompileSource(*source, isolate);
      if (!compiled.Succeeded()) {
        ReportCompileFailure(state, compiled);
        return;
      }
      const auto* entry = compiled.program.FindFunction("main");
      if (entry == nullptr || entry->parameter_count != 0) {
        state.SkipWithError("Benchmark requires a zero-argument main function");
        return;
      }
      const auto entry_index =
          static_cast<std::uint32_t>(entry - compiled.program.functions.data());
      vm::VirtualMachine machine(isolate);
      state.ResumeTiming();
      auto result = machine.Execute(compiled.program.View(), entry_index);
      benchmark::DoNotOptimize(result);
      state.PauseTiming();
      if (!IsExpectedResult(result, expected)) {
        state.SkipWithError("First execution failed or returned an unexpected result");
        return;
      }
    }
    state.ResumeTiming();
  }
}

void SteadyExecution(benchmark::State& state, std::string_view filename, std::int64_t expected) {
  const auto source = LoadSource(state, filename);
  if (!source) {
    return;
  }
  runtime::Isolate isolate;
  const auto compiled = compiler::CompileSource(*source, isolate);
  if (!compiled.Succeeded()) {
    ReportCompileFailure(state, compiled);
    return;
  }
  const auto* entry = compiled.program.FindFunction("main");
  if (entry == nullptr || entry->parameter_count != 0) {
    state.SkipWithError("Benchmark requires a zero-argument main function");
    return;
  }
  const auto entry_index = static_cast<std::uint32_t>(entry - compiled.program.functions.data());
  const auto program = compiled.program.View();
  vm::VirtualMachine machine(isolate);
  const auto first = machine.Execute(program, entry_index);
  if (!IsExpectedResult(first, expected)) {
    state.SkipWithError("Warm-up execution failed or returned an unexpected result");
    return;
  }
  for (auto iteration : state) {
    auto result = machine.Execute(program, entry_index);
    benchmark::DoNotOptimize(result);
    if (!IsExpectedResult(result, expected)) {
      state.SkipWithError("Steady execution failed or returned an unexpected result");
      break;
    }
  }
  const auto& heap = isolate.heap();
  state.counters["heap_objects"] = static_cast<double>(heap.object_count());
  state.counters["allocated_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["committed_bytes"] = static_cast<double>(heap.committed_bytes());
  state.counters["result"] = static_cast<double>(expected);
}

BENCHMARK_CAPTURE(Compile, IntegerLoop, "integer_loop.vela")->Name("compile/integer_loop");
BENCHMARK_CAPTURE(Compile, FunctionCalls, "function_calls.vela")->Name("compile/function_calls");
BENCHMARK_CAPTURE(Compile, FieldAccess, "field_access.vela")->Name("compile/field_access");
BENCHMARK_CAPTURE(Compile, AllocationGc, "allocation_gc.vela")->Name("compile/allocation_gc");

BENCHMARK_CAPTURE(FirstExecution, IntegerLoop, "integer_loop.vela", 499500)
    ->Name("first_execute/integer_loop");
BENCHMARK_CAPTURE(FirstExecution, FunctionCalls, "function_calls.vela", 1000)
    ->Name("first_execute/function_calls");
BENCHMARK_CAPTURE(FirstExecution, FieldAccess, "field_access.vela", 1000)
    ->Name("first_execute/field_access");
BENCHMARK_CAPTURE(FirstExecution, AllocationGc, "allocation_gc.vela", 1000)
    ->Name("first_execute/allocation_gc");

BENCHMARK_CAPTURE(SteadyExecution, IntegerLoop, "integer_loop.vela", 499500)
    ->Name("vm/integer_loop");
BENCHMARK_CAPTURE(SteadyExecution, FunctionCalls, "function_calls.vela", 1000)
    ->Name("vm/function_calls");
BENCHMARK_CAPTURE(SteadyExecution, FieldAccess, "field_access.vela", 1000)->Name("vm/field_access");
BENCHMARK_CAPTURE(SteadyExecution, AllocationGc, "allocation_gc.vela", 1000)
    ->Name("vm/allocation_gc");

}  // namespace
}  // namespace vela::benchmarks
