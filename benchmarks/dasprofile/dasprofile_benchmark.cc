#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include <benchmark/benchmark.h>

#include "base/file.h"
#include "base/path.h"
#include "benchmarks/aot_benchmark_memory.h"
#include "benchmarks/dasprofile/workloads.h"
#include "benchmarks/dasprofile_native/entries.h"
#include "compiler/compiler.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_call.h"
#include "runtime/native_function.h"
#include "runtime/native_instance.h"
#include "runtime/value.h"
#include "vm/vm.h"

namespace vela::benchmarks::dasprofile {
namespace {

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

void Native(benchmark::State& state, Case problem) {
  runtime::Isolate isolate;
  if (!isolate.RegisterNative(runtime::NativeFunction::Bind<AddOne>("dasprofile.addOne"))) {
    state.SkipWithError("C++ binding registration failed");
    return;
  }
  const auto instance = runtime::NativeInstance::Instantiate(isolate, *kImage);
  if (!instance) {
    state.SkipWithError(instance.error().message);
    return;
  }
  runtime::NativeExecution execution(**instance);
  const auto input = std::to_array({runtime::EncodeNativeValue(
      runtime::Value::Integer(static_cast<std::int64_t>(problem.workload)))});
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
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  state.counters["baseline_bytes"] = static_cast<double>(isolate.heap().allocated_bytes());
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

void Vm(benchmark::State& state, Case problem) {
  runtime::Isolate isolate;
  if (!isolate.RegisterNative(runtime::NativeFunction::Bind<AddOne>("dasprofile.addOne"))) {
    state.SkipWithError("C++ binding registration failed");
    return;
  }
  const auto bytes = ReadBinaryFile(Utf8Path(VELA_DASPROFILE_BENCHMARK_SOURCE));
  if (!bytes) {
    state.SkipWithError(bytes.error());
    return;
  }
  const void* storage = bytes->data();
  std::string source(static_cast<const char*>(storage), bytes->size());
  // VM 宿主入口不接受实参；只给计时外的 prepare 补一个无参入口，不改写工作负载。
  source += "\nfunc prepareVmInput(): int { return prepare(" +
            std::to_string(static_cast<std::int64_t>(problem.workload)) + "); }\n";
  const auto compiled = compiler::CompileSource(source, isolate);
  if (!compiled.Succeeded()) {
    state.SkipWithError(compiled.diagnostics.front().message);
    return;
  }
  vm::VirtualMachine machine(isolate);
  const auto prepared = machine.Execute(compiled.program.FindFunction("prepareVmInput"));
  if (!prepared.Succeeded()) {
    state.SkipWithError(prepared.message);
    return;
  }
  const auto entry = compiled.program.FindFunction(problem.name);
  const auto warm = machine.Execute(entry);
  if (!warm.Succeeded()) {
    state.SkipWithError(warm.message);
    return;
  }
  const auto number = Number(warm.value);
  if (!IsExpected(number, problem.expected)) {
    state.SkipWithError("VM warm-up checksum mismatch");
    return;
  }
  state.counters["result"] = number;
  state.counters["expected_result"] = problem.expected;
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  state.counters["baseline_bytes"] = static_cast<double>(isolate.heap().allocated_bytes());
  state.counters["register_bytes"] = static_cast<double>(machine.register_bytes());
  state.counters["suspended_frames"] =
      static_cast<double>(isolate.scheduler().suspended_frame_count());
  state.counters["saved_state_bytes"] =
      static_cast<double>(isolate.scheduler().saved_state_bytes());
  auto expected = problem.expected;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto result = machine.Execute(entry);
    if (!result.Succeeded()) {
      state.SkipWithError(result.message);
      break;
    }
    auto value = Number(result.value);
    benchmark::DoNotOptimize(value);
    expected += problem.increment;
    if (!IsExpected(value, expected)) {
      state.SkipWithError("VM checksum mismatch");
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
    benchmark::RegisterBenchmark(("aot/native/" + suffix).c_str(), Native, problem)->UseRealTime();
    benchmark::RegisterBenchmark(("aot/vm/" + suffix).c_str(), Vm, problem)->UseRealTime();
    benchmark::RegisterBenchmark(("aot/cpp/" + suffix).c_str(), Cpp, problem)->UseRealTime();
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = Register();

}  // namespace
}  // namespace vela::benchmarks::dasprofile
