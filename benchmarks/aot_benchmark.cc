#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "benchmarks/aot_benchmark_memory.h"
#include "benchmarks/aot_benchmark_types.h"
#include "benchmarks/aot_native/entries.h"
#include "compiler/compiler.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_call.h"
#include "runtime/native_function.h"
#include "runtime/native_image.h"
#include "runtime/native_instance.h"
#include "runtime/value.h"
#include "vm/vm.h"

namespace vela::benchmarks {
namespace {

std::int64_t WorkloadSize([[maybe_unused]] runtime::NativeContext& context) noexcept {
  return kAotBenchmarkCases.front().size;
}

[[clang::noinline]] std::int64_t HostStep([[maybe_unused]] runtime::NativeContext& context,
                                          std::int64_t value) noexcept {
  return value + 1;
}

[[clang::noinline]] std::int64_t PlainStep(std::int64_t value) {
  return value + 1;
}

std::expected<void, std::string> RegisterHost(runtime::Isolate& isolate) {
  if (!isolate.RegisterNative(runtime::NativeFunction::Bind<WorkloadSize>("benchmark.size")) ||
      !isolate.RegisterNative(runtime::NativeFunction::Bind<HostStep>("benchmark.step"))) {
    return std::unexpected("benchmark host registration failed");
  }
  return {};
}

struct Sample final {
  std::int64_t value = 0;
  std::size_t suspended_frames = 0;
  std::size_t saved_bytes = 0;
};

std::expected<Sample, std::string> ResolveResult(runtime::Isolate& isolate,
                                                 runtime::Value value,
                                                 AotCompletion completion) {
  Sample sample;
  auto& scheduler = isolate.scheduler();
  if (completion == AotCompletion::Future) {
    runtime::RootRange root(isolate.heap(), &value, 1);
    auto* future = value.IsObject() ? runtime::Heap::GetFuture(*value.AsObject()) : nullptr;
    if (future == nullptr) {
      return std::unexpected("async benchmark returned a non-Future");
    }
    sample.suspended_frames = scheduler.suspended_frame_count();
    sample.saved_bytes = scheduler.saved_state_bytes();
    if (!scheduler.PumpUntil(*future) || future->state() != runtime::FutureState::Succeeded) {
      return std::unexpected("async benchmark failed or became idle");
    }
    value = future->value().value_or(runtime::Value::Null());
  }
  if (!value.IsInteger()) {
    return std::unexpected("benchmark returned a non-int checksum");
  }
  sample.value = value.AsInteger();
  return sample;
}

std::expected<Sample, std::string> RunNative(runtime::NativeExecution& execution,
                                             const AotBenchmarkCase& problem) {
  const auto value = runtime::InvokeNativeTarget(execution, problem.function, {});
  if (!value) {
    return std::unexpected(execution.HasException() ? execution.ExceptionDiagnostic().message
                                                    : "native benchmark execution failed");
  }
  return ResolveResult(execution.isolate(), *value, problem.completion);
}

void ReportSample(benchmark::State& state, Sample sample) {
  state.counters["result"] = static_cast<double>(sample.value);
  state.counters["suspended_frames"] = static_cast<double>(sample.suspended_frames);
  state.counters["saved_state_bytes"] = static_cast<double>(sample.saved_bytes);
}

void NativeSteady(benchmark::State& state, AotBenchmarkCase problem) {
  runtime::Isolate isolate;
  if (const auto registered = RegisterHost(isolate); !registered) {
    state.SkipWithError(registered.error());
    return;
  }
  const auto instance = runtime::NativeInstance::Instantiate(isolate, *kAotBenchmarkImage);
  if (!instance) {
    state.SkipWithError(instance.error().message);
    return;
  }
  runtime::NativeExecution execution(**instance);
  const auto warm = RunNative(execution, problem);
  if (!warm || warm->value != problem.expected) {
    state.SkipWithError(warm ? "native warm-up checksum mismatch" : warm.error());
    return;
  }
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  state.counters["baseline_bytes"] = static_cast<double>(isolate.heap().allocated_bytes());
  ReportSample(state, *warm);
  state.counters["entry_shadow_slots"] = static_cast<double>(problem.shadow_slots);
  state.counters["entry_frame_bytes"] = static_cast<double>(problem.frame_bytes);
  state.counters["entry_static_root_stores"] = static_cast<double>(problem.root_stores);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = RunNative(execution, problem);
    if (!result || result->value != problem.expected) {
      state.SkipWithError(result ? "native checksum mismatch" : result.error());
      return;
    }
    benchmark::DoNotOptimize(result->value);
  }
  ReportAotHeap(state, isolate);
}

void VmSteady(benchmark::State& state, AotBenchmarkCase problem) {
  runtime::Isolate isolate;
  if (const auto registered = RegisterHost(isolate); !registered) {
    state.SkipWithError(registered.error());
    return;
  }
  const auto compiled = compiler::CompileFile(VELA_AOT_BENCHMARK_SOURCE, isolate);
  if (!compiled.Succeeded()) {
    state.SkipWithError(compiled.diagnostics.front().message);
    return;
  }
  vm::VirtualMachine machine(isolate);
  const auto entry = compiled.program.FindFunction(problem.name);
  const auto invoke = [&]() -> std::expected<Sample, std::string> {
    const auto result = machine.Execute(entry);
    if (!result.Succeeded()) {
      return std::unexpected(result.message);
    }
    return ResolveResult(isolate, result.value, problem.completion);
  };
  const auto warm = invoke();
  if (!warm || warm->value != problem.expected) {
    state.SkipWithError(warm ? "VM warm-up checksum mismatch" : warm.error());
    return;
  }
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  state.counters["baseline_bytes"] = static_cast<double>(isolate.heap().allocated_bytes());
  state.counters["register_bytes"] = static_cast<double>(machine.register_bytes());
  ReportSample(state, *warm);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = invoke();
    if (!result || result->value != problem.expected) {
      state.SkipWithError(result ? "VM checksum mismatch" : result.error());
      return;
    }
    benchmark::DoNotOptimize(result->value);
  }
  ReportAotHeap(state, isolate);
}

std::expected<Sample, std::string> CppAsync(runtime::Isolate& isolate, std::int64_t size) {
  auto& scheduler = isolate.scheduler();
  runtime::Future result(&scheduler);
  std::int64_t index = 0;
  std::int64_t sum = 0;
  std::optional<runtime::FutureCompletionError> completion_error;
  std::function<void(std::int64_t)> advance;
  advance = [&](std::int64_t value) {
    sum += value;
    if (index == size) {
      const auto completed = result.Complete(runtime::Value::Integer(sum));
      if (!completed) {
        completion_error = completed.error();
      }
      return;
    }
    const auto source = runtime::Future::Value(runtime::Value::Integer(++index), &scheduler);
    source.Observe([&](runtime::FutureState, runtime::Value completed) {
      const auto segment = runtime::Future::Value(completed, &scheduler);
      segment.Observe(
          [&](runtime::FutureState, runtime::Value awaited) { advance(awaited.AsInteger()); });
    });
  };
  advance(0);
  if (!scheduler.PumpUntil(result) || completion_error) {
    return std::unexpected("C++ async benchmark became idle");
  }
  const auto value = result.value().value_or(runtime::Value::Null());
  if (!value.IsInteger()) {
    return std::unexpected("C++ async benchmark returned a non-int checksum");
  }
  return Sample{.value = value.AsInteger()};
}

std::int64_t CppSpectralNorm() {
  // 与 Vela 相同的 binary64、int64 索引、500 阶矩阵和两次幂迭代。
  constexpr std::int64_t kDimension = 500;
  std::vector<double> u(kDimension, 1.0);
  std::vector<double> v(kDimension, 0.0);
  std::vector<double> temporary(kDimension, 0.0);
  const auto multiply = [](const std::vector<double>& input, std::vector<double>& output,
                           bool transpose) {
    for (std::int64_t i = 0; i < kDimension; ++i) {
      double sum = 0.0;
      for (std::int64_t j = 0; j < kDimension; ++j) {
        const auto index = transpose ? j : i;
        const auto denominator = (i + j) * (i + j + 1) / 2 + index + 1;
        sum += input[static_cast<std::size_t>(j)] / static_cast<double>(denominator);
      }
      output[static_cast<std::size_t>(i)] = sum;
    }
  };
  for (std::int64_t round = 0; round < 2; ++round) {
    multiply(u, temporary, false);
    multiply(temporary, v, true);
    multiply(v, temporary, false);
    multiply(temporary, u, true);
  }
  double numerator = 0.0;
  double denominator = 0.0;
  for (std::int64_t i = 0; i < kDimension; ++i) {
    const auto index = static_cast<std::size_t>(i);
    numerator += u[index] * v[index];
    denominator += v[index] * v[index];
  }
  return static_cast<std::int64_t>(std::sqrt(numerator / denominator) * 1000000000.0);
}

std::expected<Sample, std::string> CppRun(runtime::Isolate& isolate, AotBenchmarkCase problem) {
  auto size = problem.size;
  benchmark::DoNotOptimize(size);
  std::int64_t sum = 0;
  struct Box final {
    std::int64_t value = 0;
    std::string text;
  };
  switch (problem.kernel) {
    case AotKernel::Arithmetic:
      for (std::int64_t index = 0; index < size; ++index) {
        sum += index;
      }
      break;
    case AotKernel::Calls:
      for (std::int64_t index = 0; index < size; ++index) {
        sum += PlainStep(index);
      }
      break;
    case AotKernel::Fields: {
      auto box = std::make_unique<Box>();
      benchmark::DoNotOptimize(box.get());
      for (std::int64_t index = 0; index < size; ++index) {
        box->value += 1;
      }
      sum = box->value;
      break;
    }
    case AotKernel::Arrays: {
      std::vector<std::int64_t> values;
      values.reserve(static_cast<std::size_t>(size));
      for (std::int64_t index = 0; index < size; ++index) {
        values.push_back(index);
      }
      for (const auto value : values) {
        sum += value;
      }
      break;
    }
    case AotKernel::Host: {
      runtime::NativeContext context(isolate);
      for (std::int64_t index = 0; index < size; ++index) {
        sum += HostStep(context, index);
      }
      break;
    }
    case AotKernel::Gc: {
      constexpr std::size_t kDecimalBufferSize = 32;
      std::array<char, kDecimalBufferSize> buffer{};
      for (std::int64_t index = 0; index < size; ++index) {
        auto box = std::make_unique<Box>();
        box->value = index;
        const auto converted = std::to_chars(buffer.data(), std::to_address(buffer.end()), index);
        box->text.assign(buffer.data(), converted.ptr);
        benchmark::DoNotOptimize(box.get());
        sum += static_cast<std::int64_t>(box->text.size());
      }
      break;
    }
    case AotKernel::Async:
      return CppAsync(isolate, size);
    case AotKernel::SpectralNorm:
      return Sample{.value = CppSpectralNorm()};
  }
  return Sample{.value = sum};
}

void CppSteady(benchmark::State& state, AotBenchmarkCase problem) {
  runtime::Isolate isolate;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = CppRun(isolate, problem);
    if (!result || result->value != problem.expected) {
      state.SkipWithError("C++ benchmark checksum mismatch");
      return;
    }
    benchmark::DoNotOptimize(result->value);
  }
  state.counters["result"] = static_cast<double>(problem.expected);
  ReportAotResident(state);
}

void NativeFirst(benchmark::State& state, AotBenchmarkCase problem) {
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    state.PauseTiming();
    {
      runtime::Isolate isolate;
      if (const auto registered = RegisterHost(isolate); !registered) {
        state.ResumeTiming();
        state.SkipWithError(registered.error());
        return;
      }
      const auto instance = runtime::NativeInstance::Instantiate(isolate, *kAotBenchmarkImage);
      if (!instance) {
        state.ResumeTiming();
        state.SkipWithError(instance.error().message);
        return;
      }
      runtime::NativeExecution execution(**instance);
      state.ResumeTiming();
      auto result = RunNative(execution, problem);
      benchmark::DoNotOptimize(result);
      state.PauseTiming();
      if (!result || result->value != problem.expected) {
        state.ResumeTiming();
        state.SkipWithError("first native execution checksum mismatch");
        return;
      }
    }
    state.ResumeTiming();
  }
}

int RegisterAotBenchmarks() {
  benchmark::AddCustomContext("aot_runtime_abi", std::string(runtime::NativeRuntimeAbiIdentity()));
  benchmark::AddCustomContext("aot_codegen",
                              "LLVM O2; generic CPU; PIC; checked arithmetic; explicit roots");
  for (const auto& problem : kAotBenchmarkCases) {
    const auto suffix = std::string(problem.name) + "/" + std::to_string(problem.size);
    benchmark::RegisterBenchmark("aot/native/" + suffix, NativeSteady, problem)->UseRealTime();
    benchmark::RegisterBenchmark("aot/vm/" + suffix, VmSteady, problem)->UseRealTime();
    benchmark::RegisterBenchmark("aot/cpp/" + suffix, CppSteady, problem)->UseRealTime();
    benchmark::RegisterBenchmark("aot/first_native/" + suffix, NativeFirst, problem)->UseRealTime();
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = RegisterAotBenchmarks();

}  // namespace
}  // namespace vela::benchmarks
