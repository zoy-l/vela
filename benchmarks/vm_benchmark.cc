#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "benchmark/benchmark.h"

#include "compiler/compiler.h"
#include "runtime/heap.h"
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
      const auto entry = compiled.program.FindFunction("main");
      if (!entry.IsValid() || entry.parameter_count() != 0) {
        state.SkipWithError("Benchmark requires a zero-argument main function");
        return;
      }
      const auto entry_index = entry.index();
      vm::VirtualMachine machine(isolate);
      state.ResumeTiming();
      auto result = machine.Execute(compiled.program, entry_index);
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
  const auto entry = compiled.program.FindFunction("main");
  if (!entry.IsValid() || entry.parameter_count() != 0) {
    state.SkipWithError("Benchmark requires a zero-argument main function");
    return;
  }
  const auto entry_index = entry.index();
  const auto program = compiled.program;
  vm::VirtualMachine machine(isolate);
  const auto first = machine.Execute(program, entry_index);
  if (!IsExpectedResult(first, expected)) {
    state.SkipWithError("Warm-up execution failed or returned an unexpected result");
    return;
  }
  auto& heap = isolate.heap();
  // 固定记录一次执行后的状态，避免把计时迭代次数造成的 GC 相位差当作内存变化。
  state.counters["first_allocated_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["first_committed_bytes"] = static_cast<double>(heap.committed_bytes());
  state.counters["register_bytes"] = static_cast<double>(machine.register_bytes());
  for (auto iteration : state) {
    auto result = machine.Execute(program, entry_index);
    benchmark::DoNotOptimize(result);
    if (!IsExpectedResult(result, expected)) {
      state.SkipWithError("Steady execution failed or returned an unexpected result");
      break;
    }
  }
  state.counters["heap_objects"] = static_cast<double>(heap.object_count());
  state.counters["allocated_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["committed_bytes"] = static_cast<double>(heap.committed_bytes());
  // 先结束可能仍在运行的增量周期，再启动无 VM 帧的新周期，排除浮动垃圾。
  heap.FinishCollection();
  heap.Collect();
  state.counters["retained_objects"] = static_cast<double>(heap.object_count());
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["result"] = static_cast<double>(expected);
}

BENCHMARK_CAPTURE(Compile, IntegerLoop, "integer_loop.vela")->Name("compile/integer_loop");
BENCHMARK_CAPTURE(Compile, SwitchDispatch, "switch_dispatch.vela")->Name("compile/switch_dispatch");
BENCHMARK_CAPTURE(Compile, IfDispatch, "if_dispatch.vela")->Name("compile/if_dispatch");
BENCHMARK_CAPTURE(Compile, NullableCalls, "nullable_calls.vela")->Name("compile/nullable_calls");
BENCHMARK_CAPTURE(FirstExecution, SwitchDispatch, "switch_dispatch.vela", 3500)
    ->Name("first_execute/switch_dispatch");
BENCHMARK_CAPTURE(FirstExecution, IfDispatch, "if_dispatch.vela", 3500)
    ->Name("first_execute/if_dispatch");
BENCHMARK_CAPTURE(FirstExecution, NullableCalls, "nullable_calls.vela", 1000)
    ->Name("first_execute/nullable_calls");
BENCHMARK_CAPTURE(SteadyExecution, SwitchDispatch, "switch_dispatch.vela", 3500)
    ->Name("vm/switch_dispatch");
BENCHMARK_CAPTURE(SteadyExecution, IfDispatch, "if_dispatch.vela", 3500)->Name("vm/if_dispatch");
BENCHMARK_CAPTURE(SteadyExecution, NullableCalls, "nullable_calls.vela", 1000)
    ->Name("vm/nullable_calls");
BENCHMARK_CAPTURE(Compile, FunctionCalls, "function_calls.vela")->Name("compile/function_calls");
BENCHMARK_CAPTURE(Compile, FieldAccess, "field_access.vela")->Name("compile/field_access");
BENCHMARK_CAPTURE(Compile, AllocationGc, "allocation_gc.vela")->Name("compile/allocation_gc");
BENCHMARK_CAPTURE(Compile, ArrayTraversal, "array_traversal.vela")->Name("compile/array_traversal");
BENCHMARK_CAPTURE(Compile, ArrayAllocation, "array_allocation.vela")
    ->Name("compile/array_allocation");
BENCHMARK_CAPTURE(Compile, DoubleLoop, "double_loop.vela")->Name("compile/double_loop");
BENCHMARK_CAPTURE(Compile, DoubleArray, "double_array.vela")->Name("compile/double_array");
BENCHMARK_CAPTURE(Compile, StringConcat, "string_concat.vela")->Name("compile/string_concat");
BENCHMARK_CAPTURE(Compile, StringGrowth, "string_growth.vela")->Name("compile/string_growth");
BENCHMARK_CAPTURE(Compile, StringPartsConcat, "string_parts_concat.vela")
    ->Name("compile/string_parts_concat");
BENCHMARK_CAPTURE(Compile, StringInterpolation, "string_interpolation.vela")
    ->Name("compile/string_interpolation");
BENCHMARK_CAPTURE(Compile, StringJoin, "string_join.vela")->Name("compile/string_join");
BENCHMARK_CAPTURE(Compile, StringBuilder, "string_builder.vela")->Name("compile/string_builder");
BENCHMARK_CAPTURE(Compile, StringBuilderLarge, "string_builder_large.vela")
    ->Name("compile/string_builder_large");
BENCHMARK_CAPTURE(Compile, StringGrowthLarge, "string_growth_large.vela")
    ->Name("compile/string_growth_large");
BENCHMARK_CAPTURE(Compile, StringUnicode, "string_unicode.vela")->Name("compile/string_unicode");

BENCHMARK_CAPTURE(FirstExecution, IntegerLoop, "integer_loop.vela", 499500)
    ->Name("first_execute/integer_loop");
BENCHMARK_CAPTURE(FirstExecution, FunctionCalls, "function_calls.vela", 1000)
    ->Name("first_execute/function_calls");
BENCHMARK_CAPTURE(FirstExecution, FieldAccess, "field_access.vela", 1000)
    ->Name("first_execute/field_access");
BENCHMARK_CAPTURE(FirstExecution, AllocationGc, "allocation_gc.vela", 1000)
    ->Name("first_execute/allocation_gc");
BENCHMARK_CAPTURE(FirstExecution, ArrayTraversal, "array_traversal.vela", 120000)
    ->Name("first_execute/array_traversal");
BENCHMARK_CAPTURE(FirstExecution, ArrayAllocation, "array_allocation.vela", 499500)
    ->Name("first_execute/array_allocation");
BENCHMARK_CAPTURE(FirstExecution, DoubleLoop, "double_loop.vela", 1000)
    ->Name("first_execute/double_loop");
BENCHMARK_CAPTURE(FirstExecution, DoubleArray, "double_array.vela", 16000)
    ->Name("first_execute/double_array");
BENCHMARK_CAPTURE(FirstExecution, StringConcat, "string_concat.vela", 1000)
    ->Name("first_execute/string_concat");
BENCHMARK_CAPTURE(FirstExecution, StringGrowth, "string_growth.vela", 512)
    ->Name("first_execute/string_growth");
BENCHMARK_CAPTURE(FirstExecution, StringPartsConcat, "string_parts_concat.vela", 1000)
    ->Name("first_execute/string_parts_concat");
BENCHMARK_CAPTURE(FirstExecution, StringInterpolation, "string_interpolation.vela", 1000)
    ->Name("first_execute/string_interpolation");
BENCHMARK_CAPTURE(FirstExecution, StringJoin, "string_join.vela", 1000)
    ->Name("first_execute/string_join");
BENCHMARK_CAPTURE(FirstExecution, StringBuilder, "string_builder.vela", 512)
    ->Name("first_execute/string_builder");
BENCHMARK_CAPTURE(FirstExecution, StringBuilderLarge, "string_builder_large.vela", 4096)
    ->Name("first_execute/string_builder_large");
BENCHMARK_CAPTURE(FirstExecution, StringGrowthLarge, "string_growth_large.vela", 4096)
    ->Name("first_execute/string_growth_large");
BENCHMARK_CAPTURE(FirstExecution, StringUnicode, "string_unicode.vela", 1000)
    ->Name("first_execute/string_unicode");

BENCHMARK_CAPTURE(SteadyExecution, IntegerLoop, "integer_loop.vela", 499500)
    ->Name("vm/integer_loop");
BENCHMARK_CAPTURE(SteadyExecution, FunctionCalls, "function_calls.vela", 1000)
    ->Name("vm/function_calls");
BENCHMARK_CAPTURE(SteadyExecution, FieldAccess, "field_access.vela", 1000)->Name("vm/field_access");
BENCHMARK_CAPTURE(SteadyExecution, AllocationGc, "allocation_gc.vela", 1000)
    ->Name("vm/allocation_gc");
BENCHMARK_CAPTURE(SteadyExecution, ArrayTraversal, "array_traversal.vela", 120000)
    ->Name("vm/array_traversal");
BENCHMARK_CAPTURE(SteadyExecution, ArrayAllocation, "array_allocation.vela", 499500)
    ->Name("vm/array_allocation");
BENCHMARK_CAPTURE(SteadyExecution, DoubleLoop, "double_loop.vela", 1000)->Name("vm/double_loop");
BENCHMARK_CAPTURE(SteadyExecution, DoubleArray, "double_array.vela", 16000)
    ->Name("vm/double_array");
BENCHMARK_CAPTURE(SteadyExecution, StringConcat, "string_concat.vela", 1000)
    ->Name("vm/string_concat");
BENCHMARK_CAPTURE(SteadyExecution, StringGrowth, "string_growth.vela", 512)
    ->Name("vm/string_growth");
BENCHMARK_CAPTURE(SteadyExecution, StringPartsConcat, "string_parts_concat.vela", 1000)
    ->Name("vm/string_parts_concat");
BENCHMARK_CAPTURE(SteadyExecution, StringInterpolation, "string_interpolation.vela", 1000)
    ->Name("vm/string_interpolation");
BENCHMARK_CAPTURE(SteadyExecution, StringJoin, "string_join.vela", 1000)->Name("vm/string_join");
BENCHMARK_CAPTURE(SteadyExecution, StringBuilder, "string_builder.vela", 512)
    ->Name("vm/string_builder");
BENCHMARK_CAPTURE(SteadyExecution, StringBuilderLarge, "string_builder_large.vela", 4096)
    ->Name("vm/string_builder_large");
BENCHMARK_CAPTURE(SteadyExecution, StringGrowthLarge, "string_growth_large.vela", 4096)
    ->Name("vm/string_growth_large");
BENCHMARK_CAPTURE(SteadyExecution, StringUnicode, "string_unicode.vela", 1000)
    ->Name("vm/string_unicode");

}  // namespace
}  // namespace vela::benchmarks
