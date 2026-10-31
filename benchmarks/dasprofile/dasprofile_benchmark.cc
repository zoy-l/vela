#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <benchmark/benchmark.h>

#include "base/native_program_mode.h"
#include "benchmarks/aot_benchmark_memory.h"
#include "benchmarks/dasprofile/workloads.h"
#include "benchmarks/dasprofile_native/entries.h"
#include "benchmarks/heap_metrics.h"
#include "benchmarks/jit_benchmark.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

namespace vela::benchmarks::dasprofile {
namespace {

constexpr const char* kAddOneBinding = "dasprofile.addOne";

bool IsExpected(double actual, double expected) {
  return std::isfinite(actual) &&
         std::abs(actual - expected) <= std::max(1e-10, std::abs(expected) * 1e-12);
}

double Number(runtime::Value value) {
  if (value.IsInteger()) {
    return static_cast<double>(value.AsInteger());
  }
  return value.IsDouble() ? value.AsDouble() : std::numeric_limits<double>::quiet_NaN();
}

void Cpp(benchmark::State& state, Case problem) {
  CppWorkload workload(problem.workload);
  const auto warm = workload.Run();
  if (!IsExpected(warm, problem.expected)) {
    state.SkipWithError("C++ warm-up checksum mismatch");
    return;
  }
  state.counters["result"] = warm;
  state.counters["expected_result"] = problem.expected;
  auto expected = problem.expected;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = workload.Run();
    benchmark::DoNotOptimize(result);
    expected += problem.increment;
    if (!IsExpected(result, expected)) {
      state.SkipWithError("C++ checksum mismatch");
      break;
    }
  }
  ReportAotResident(state);
}

void Jit(benchmark::State& state, Case problem, NativeCodeTier tier) {
  runtime::NativeBindings bindings;
  {
    runtime::Isolate isolate;
    if (!isolate.RegisterNative(runtime::NativeFunction::Bind<AddOne>(kAddOneBinding))) {
      state.SkipWithError("C++ binding registration failed");
      return;
    }
    bindings = isolate.FreezeBindings();
  }
  auto setup =
      LoadJitWorkload(VELA_DASPROFILE_BENCHMARK_SOURCE, std::move(bindings), {.tier = tier});
  if (!setup) {
    state.SkipWithError(setup.error());
    return;
  }
  auto& session = *setup->session;
  const auto snapshot = session.snapshot();
  const auto input =
      std::to_array({runtime::Value::Integer(static_cast<std::int64_t>(problem.workload))});
  if (!session.Invoke(snapshot->roots.front(), "prepare", input)) {
    state.SkipWithError("JIT input preparation failed");
    return;
  }
  const auto first_started = std::chrono::steady_clock::now();
  const auto warm = InvokeJitWorkload(session, problem.name);
  ReportJitFirstExecution(state, first_started);
  if (!warm || !IsExpected(Number(*warm), problem.expected)) {
    state.SkipWithError("JIT warm-up checksum mismatch");
    return;
  }
  CollectHeapBaseline(state, session.isolate().heap());
  state.counters["result"] = problem.expected;
  state.counters["expected_result"] = problem.expected;
  auto expected = problem.expected;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto result = InvokeJitWorkload(session, problem.name);
    if (!result) {
      state.SkipWithError("JIT execution failed");
      return;
    }
    auto value = Number(*result);
    benchmark::DoNotOptimize(value);
    expected += problem.increment;
    if (!IsExpected(value, expected)) {
      state.SkipWithError("JIT checksum mismatch");
      return;
    }
  }
  ReportJitSetup(state, *setup);
  ReportJitMemory(state, session);
}

void Native(benchmark::State& state, Case problem) {
  runtime::Isolate isolate;
  if (!isolate.RegisterNative(runtime::NativeFunction::Bind<AddOne>(kAddOneBinding))) {
    state.SkipWithError("C++ binding registration failed");
    return;
  }
  const auto instance = runtime::NativeInstance::Instantiate(isolate, *kImage);
  if (!instance) {
    state.SkipWithError(instance.error().message);
    return;
  }
  runtime::NativeExecution execution(**instance);
  const auto input = std::to_array({
      runtime::EncodeNativeValue(
          runtime::Value::Integer(static_cast<std::int64_t>(problem.workload))),
  });
  if (!runtime::InvokeNativeTarget(execution, kPrepare, input)) {
    state.SkipWithError(execution.ExceptionDiagnostic().message);
    return;
  }
  const auto warm = runtime::InvokeNativeTarget(execution, problem.function, {});
  if (!warm) {
    state.SkipWithError(execution.ExceptionDiagnostic().message);
    return;
  }
  const auto number = Number(*warm);
  if (!IsExpected(number, problem.expected)) {
    state.SkipWithError("AOT warm-up checksum mismatch");
    return;
  }
  state.counters["result"] = number;
  state.counters["expected_result"] = problem.expected;
  CollectHeapBaseline(state, isolate.heap());
  state.counters["suspended_frames"] =
      static_cast<double>(isolate.scheduler().suspended_frame_count());
  state.counters["saved_state_bytes"] =
      static_cast<double>(isolate.scheduler().saved_state_bytes());
  state.counters["entry_shadow_slots"] = static_cast<double>(problem.shadow_slots);
  state.counters["entry_frame_bytes"] = static_cast<double>(problem.frame_bytes);
  state.counters["entry_static_root_stores"] = static_cast<double>(problem.root_stores);
  auto expected = problem.expected;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto result = runtime::InvokeNativeTarget(execution, problem.function, {});
    if (!result) {
      state.SkipWithError(execution.ExceptionDiagnostic().message);
      break;
    }
    auto value = Number(*result);
    benchmark::DoNotOptimize(value);
    expected += problem.increment;
    if (!IsExpected(value, expected)) {
      state.SkipWithError("AOT checksum mismatch");
      break;
    }
  }
  ReportAotHeap(state, isolate);
}

int Register() {
  benchmark::AddCustomContext("dasprofile_upstream", "https://github.com/borisbat/dasProfile");
  benchmark::AddCustomContext("dasprofile_revision", "436d58d2840ff839550d44d32f98e10a15b57bf2");
  benchmark::AddCustomContext("dasprofile_cpp",
                              "GN Release flags; FP contraction disabled; stable_sort");
  for (const auto& problem : kCases) {
    const auto suffix =
        "dasprofile." + std::string(problem.name) + "/" + std::to_string(problem.size);
    benchmark::RegisterBenchmark("aot/native/" + suffix, Native, problem)->UseRealTime();
    benchmark::RegisterBenchmark("aot/cpp/" + suffix, Cpp, problem)->UseRealTime();
    benchmark::RegisterBenchmark("hot/steady/baseline/" + suffix, Jit, problem,
                                 NativeCodeTier::Baseline)
        ->UseRealTime();
    benchmark::RegisterBenchmark("hot/steady/optimized/" + suffix, Jit, problem,
                                 NativeCodeTier::Optimized)
        ->UseRealTime();
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = Register();

}  // namespace
}  // namespace vela::benchmarks::dasprofile
