#include <cstdint>
#include <string_view>

#include "benchmark/benchmark.h"

#include "benchmarks/heap_metrics.h"
#include "benchmarks/source.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_execution.h"

namespace vela::benchmarks {
namespace {

bool IsExpectedResult(const runtime::ExecutionResult& result, std::int64_t expected) {
  return result.Succeeded() && result.value.IsInteger() && result.value.AsInteger() == expected;
}

void Compile(benchmark::State& state, std::string_view filename) {
  const auto source = LoadBenchmarkSource(state, filename);
  if (!source) {
    return;
  }
  for (auto iteration : state) {
    auto compiler = compiler::jit::Compiler::Create();
    if (!compiler) {
      state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
      return;
    }
    auto compiled = compiler::jit::CompileSource(**compiler, *source);
    if (!compiled.Succeeded()) {
      state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
      break;
    }
    benchmark::DoNotOptimize(compiled);
  }
}

void FirstExecution(benchmark::State& state, std::string_view filename, std::int64_t expected) {
  const auto source = LoadBenchmarkSource(state, filename);
  if (!source) {
    return;
  }
  for (auto iteration : state) {
    state.PauseTiming();
    {
      // 每次使用新的 Isolate 和原生实例；编译、验证及销毁不计入首次执行时间。
      runtime::Isolate isolate;
      const auto instance = CompileBenchmarkSource(state, *source, isolate);
      if (!instance) {
        return;
      }
      const auto entry_index = *instance->FindFunction("main");
      state.ResumeTiming();
      auto result = runtime::ExecuteNativeFunction(*instance, entry_index);
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
  const auto source = LoadBenchmarkSource(state, filename);
  if (!source) {
    return;
  }
  runtime::Isolate isolate;
  const auto instance = CompileBenchmarkSource(state, *source, isolate);
  if (!instance) {
    return;
  }
  const auto entry_index = *instance->FindFunction("main");
  const auto first = runtime::ExecuteNativeFunction(*instance, entry_index);
  if (!IsExpectedResult(first, expected)) {
    state.SkipWithError("Warm-up execution failed or returned an unexpected result");
    return;
  }
  auto& heap = isolate.heap();
  // 固定记录一次执行后的状态，避免把计时迭代次数造成的 GC 相位差当作内存变化。
  state.counters["first_allocated_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["first_committed_bytes"] = static_cast<double>(heap.committed_bytes());
  for (auto iteration : state) {
    auto result = runtime::ExecuteNativeFunction(*instance, entry_index);
    benchmark::DoNotOptimize(result);
    if (!IsExpectedResult(result, expected)) {
      state.SkipWithError("Steady execution failed or returned an unexpected result");
      break;
    }
  }
  state.counters["heap_objects"] = static_cast<double>(heap.object_count());
  ReportHeap(state, heap);
  state.counters["retained_objects"] = static_cast<double>(heap.object_count());
  state.counters["result"] = static_cast<double>(expected);
}

BENCHMARK_CAPTURE(Compile, IntegerLoop, "integer_loop.vela")->Name("compile/integer_loop");
BENCHMARK_CAPTURE(Compile, ExceptionCaught, "exception_caught.vela")
    ->Name("compile/exception_caught");
BENCHMARK_CAPTURE(FirstExecution, ExceptionCaught, "exception_caught.vela", 499500)
    ->Name("first_execute/exception_caught");
BENCHMARK_CAPTURE(SteadyExecution, ExceptionCaught, "exception_caught.vela", 499500)
    ->Name("jit/exception_caught");
BENCHMARK_CAPTURE(Compile, ExceptionNoThrow, "exception_no_throw.vela")
    ->Name("compile/exception_no_throw");
BENCHMARK_CAPTURE(FirstExecution, ExceptionNoThrow, "exception_no_throw.vela", 499500)
    ->Name("first_execute/exception_no_throw");
BENCHMARK_CAPTURE(SteadyExecution, ExceptionNoThrow, "exception_no_throw.vela", 499500)
    ->Name("jit/exception_no_throw");
BENCHMARK_CAPTURE(Compile, SetMembership, "set_membership.vela")->Name("compile/set_membership");
BENCHMARK_CAPTURE(Compile, MapLookup, "map_lookup.vela")->Name("compile/map_lookup");
BENCHMARK_CAPTURE(Compile, InterfaceCalls, "interface_calls.vela")->Name("compile/interface_calls");
BENCHMARK_CAPTURE(Compile, MatchDispatch, "match_dispatch.vela")->Name("compile/match_dispatch");
BENCHMARK_CAPTURE(FirstExecution, SetMembership, "set_membership.vela", 499500)
    ->Name("first_execute/set_membership");
BENCHMARK_CAPTURE(FirstExecution, MapLookup, "map_lookup.vela", 499500)
    ->Name("first_execute/map_lookup");
BENCHMARK_CAPTURE(FirstExecution, InterfaceCalls, "interface_calls.vela", 1000)
    ->Name("first_execute/interface_calls");
BENCHMARK_CAPTURE(FirstExecution, MatchDispatch, "match_dispatch.vela", 3500)
    ->Name("first_execute/match_dispatch");
BENCHMARK_CAPTURE(SteadyExecution, SetMembership, "set_membership.vela", 499500)
    ->Name("jit/set_membership");
BENCHMARK_CAPTURE(SteadyExecution, MapLookup, "map_lookup.vela", 499500)->Name("jit/map_lookup");
BENCHMARK_CAPTURE(SteadyExecution, InterfaceCalls, "interface_calls.vela", 1000)
    ->Name("jit/interface_calls");
BENCHMARK_CAPTURE(SteadyExecution, MatchDispatch, "match_dispatch.vela", 3500)
    ->Name("jit/match_dispatch");
BENCHMARK_CAPTURE(Compile, NumericConversions, "numeric_conversions.vela")
    ->Name("compile/numeric_conversions");
BENCHMARK_CAPTURE(Compile, ClosureCalls, "closure_calls.vela")->Name("compile/closure_calls");
BENCHMARK_CAPTURE(Compile, UnicodeGraphemes, "unicode_graphemes.vela")
    ->Name("compile/unicode_graphemes");
BENCHMARK_CAPTURE(FirstExecution, UnicodeGraphemes, "unicode_graphemes.vela", 4000)
    ->Name("first_execute/unicode_graphemes");
BENCHMARK_CAPTURE(SteadyExecution, UnicodeGraphemes, "unicode_graphemes.vela", 4000)
    ->Name("jit/unicode_graphemes");
BENCHMARK_CAPTURE(FirstExecution, ClosureCalls, "closure_calls.vela", 1000)
    ->Name("first_execute/closure_calls");
BENCHMARK_CAPTURE(SteadyExecution, ClosureCalls, "closure_calls.vela", 1000)
    ->Name("jit/closure_calls");
BENCHMARK_CAPTURE(FirstExecution, NumericConversions, "numeric_conversions.vela", 499500)
    ->Name("first_execute/numeric_conversions");
BENCHMARK_CAPTURE(SteadyExecution, NumericConversions, "numeric_conversions.vela", 499500)
    ->Name("jit/numeric_conversions");
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
    ->Name("jit/switch_dispatch");
BENCHMARK_CAPTURE(SteadyExecution, IfDispatch, "if_dispatch.vela", 3500)->Name("jit/if_dispatch");
BENCHMARK_CAPTURE(SteadyExecution, NullableCalls, "nullable_calls.vela", 1000)
    ->Name("jit/nullable_calls");
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
BENCHMARK_CAPTURE(Compile, StringFormat, "string_format.vela")->Name("compile/string_format");
BENCHMARK_CAPTURE(Compile, StringFormatExplicit, "string_format_explicit.vela")
    ->Name("compile/string_format_explicit");
BENCHMARK_CAPTURE(Compile, NumberJoin, "number_join.vela")->Name("compile/number_join");
BENCHMARK_CAPTURE(Compile, MutableString, "string_builder.vela")->Name("compile/string_builder");
BENCHMARK_CAPTURE(Compile, MutableStringLarge, "string_builder_large.vela")
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
BENCHMARK_CAPTURE(FirstExecution, StringFormat, "string_format.vela", 1000)
    ->Name("first_execute/string_format");
BENCHMARK_CAPTURE(FirstExecution, StringFormatExplicit, "string_format_explicit.vela", 1000)
    ->Name("first_execute/string_format_explicit");
BENCHMARK_CAPTURE(FirstExecution, NumberJoin, "number_join.vela", 1000)
    ->Name("first_execute/number_join");
BENCHMARK_CAPTURE(FirstExecution, MutableString, "string_builder.vela", 512)
    ->Name("first_execute/string_builder");
BENCHMARK_CAPTURE(FirstExecution, MutableStringLarge, "string_builder_large.vela", 4096)
    ->Name("first_execute/string_builder_large");
BENCHMARK_CAPTURE(FirstExecution, StringGrowthLarge, "string_growth_large.vela", 4096)
    ->Name("first_execute/string_growth_large");
BENCHMARK_CAPTURE(FirstExecution, StringUnicode, "string_unicode.vela", 1000)
    ->Name("first_execute/string_unicode");

BENCHMARK_CAPTURE(SteadyExecution, IntegerLoop, "integer_loop.vela", 499500)
    ->Name("jit/integer_loop");
BENCHMARK_CAPTURE(Compile, StandardApi, "library_linking.vela")->Name("compile/standard_api");
BENCHMARK_CAPTURE(SteadyExecution, StandardApi, "library_linking.vela", 6)
    ->Name("jit/standard_api");
BENCHMARK_CAPTURE(Compile, ArrayAlgorithms, "array_algorithms.vela")
    ->Name("compile/array_algorithms");
BENCHMARK_CAPTURE(SteadyExecution, ArrayAlgorithms, "array_algorithms.vela", 499500)
    ->Name("jit/array_algorithms");
BENCHMARK_CAPTURE(Compile, SetOperations, "set_operations.vela")->Name("compile/set_operations");
BENCHMARK_CAPTURE(SteadyExecution, SetOperations, "set_operations.vela", 640)
    ->Name("jit/set_operations");
BENCHMARK_CAPTURE(SteadyExecution, FunctionCalls, "function_calls.vela", 1000)
    ->Name("jit/function_calls");
BENCHMARK_CAPTURE(SteadyExecution, FieldAccess, "field_access.vela", 1000)
    ->Name("jit/field_access");
BENCHMARK_CAPTURE(SteadyExecution, AllocationGc, "allocation_gc.vela", 1000)
    ->Name("jit/allocation_gc");
BENCHMARK_CAPTURE(SteadyExecution, ArrayTraversal, "array_traversal.vela", 120000)
    ->Name("jit/array_traversal");
BENCHMARK_CAPTURE(SteadyExecution, ArrayAllocation, "array_allocation.vela", 499500)
    ->Name("jit/array_allocation");
BENCHMARK_CAPTURE(SteadyExecution, DoubleLoop, "double_loop.vela", 1000)->Name("jit/double_loop");
BENCHMARK_CAPTURE(SteadyExecution, DoubleArray, "double_array.vela", 16000)
    ->Name("jit/double_array");
BENCHMARK_CAPTURE(SteadyExecution, StringConcat, "string_concat.vela", 1000)
    ->Name("jit/string_concat");
BENCHMARK_CAPTURE(SteadyExecution, StringGrowth, "string_growth.vela", 512)
    ->Name("jit/string_growth");
BENCHMARK_CAPTURE(SteadyExecution, StringPartsConcat, "string_parts_concat.vela", 1000)
    ->Name("jit/string_parts_concat");
BENCHMARK_CAPTURE(SteadyExecution, StringInterpolation, "string_interpolation.vela", 1000)
    ->Name("jit/string_interpolation");
BENCHMARK_CAPTURE(SteadyExecution, StringJoin, "string_join.vela", 1000)->Name("jit/string_join");
BENCHMARK_CAPTURE(SteadyExecution, StringFormat, "string_format.vela", 1000)
    ->Name("jit/string_format");
BENCHMARK_CAPTURE(SteadyExecution, StringFormatExplicit, "string_format_explicit.vela", 1000)
    ->Name("jit/string_format_explicit");
BENCHMARK_CAPTURE(SteadyExecution, NumberJoin, "number_join.vela", 1000)->Name("jit/number_join");
BENCHMARK_CAPTURE(SteadyExecution, MutableString, "string_builder.vela", 512)
    ->Name("jit/string_builder");
BENCHMARK_CAPTURE(SteadyExecution, MutableStringLarge, "string_builder_large.vela", 4096)
    ->Name("jit/string_builder_large");
BENCHMARK_CAPTURE(SteadyExecution, StringGrowthLarge, "string_growth_large.vela", 4096)
    ->Name("jit/string_growth_large");
BENCHMARK_CAPTURE(SteadyExecution, StringUnicode, "string_unicode.vela", 1000)
    ->Name("jit/string_unicode");

}  // namespace
}  // namespace vela::benchmarks
