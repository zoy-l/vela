#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "benchmarks/algorithms/workloads.h"
#include "benchmarks/aot_benchmark_memory.h"
#include "benchmarks/aot_benchmark_types.h"
#include "benchmarks/aot_native/entries.h"
#include "benchmarks/heap_metrics.h"
#include "benchmarks/jit_benchmark.h"
#include "compiler/native/code_generator.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_stack.h"
#include "runtime/value/value.h"

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

std::int64_t ArithmeticSum(std::int64_t size) {
  std::int64_t sum = 0;
  for (std::int64_t index = 0; index < size; ++index) {
    sum += index;
  }
  return sum;
}

[[clang::noinline]] std::int64_t ArithmeticEntry(std::int64_t size) {
  benchmark::DoNotOptimize(size);
  return ArithmeticSum(size);
}

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
    if (scheduler.PumpUntil(*future) == runtime::FuturePumpResult::Idle ||
        future->state() != runtime::FutureState::Succeeded) {
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
  CollectHeapBaseline(state, isolate.heap());
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

enum class EntryCallLayer : std::uint8_t { Generated, Target };

template <EntryCallLayer Layer>
void NativeEntryLayer(benchmark::State& state, AotBenchmarkCase problem) {
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
  const auto warm = runtime::InvokeNativeTarget(execution, problem.function, {});
  if (!warm || !warm->IsInteger() || warm->AsInteger() != problem.expected) {
    state.SkipWithError("entry warm-up checksum mismatch");
    return;
  }
  auto entry = (*instance)->image().function(problem.function).entry;
  benchmark::DoNotOptimize(entry);
  CollectHeapBaseline(state, isolate.heap());
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    runtime::Value value;
    if constexpr (Layer == EntryCallLayer::Generated) {
      runtime::NativeValue output;
      const auto status = entry(&execution, nullptr, 0, &output);
      if (status != runtime::ToNativeStatus(ExecutionStatus::Returned)) {
        state.SkipWithError("generated entry failed");
        return;
      }
      value = runtime::DecodeNativeValue(output);
    } else {
      const auto result = runtime::InvokeNativeTarget(execution, problem.function, {});
      if (!result) {
        state.SkipWithError("native target failed");
        return;
      }
      value = *result;
    }
    if (!value.IsInteger() || value.AsInteger() != problem.expected) {
      state.SkipWithError("entry checksum mismatch");
      return;
    }
    benchmark::DoNotOptimize(value.AsInteger());
  }
  state.counters["result"] = static_cast<double>(problem.expected);
  state.counters["expected_result"] = static_cast<double>(problem.expected);
  ReportAotHeap(state, isolate);
}

enum class CppEntryCall : std::uint8_t { Direct, Indirect };

template <CppEntryCall Call>
void CppArithmeticEntry(benchmark::State& state) {
  auto size = kAotBenchmarkCases.front().size;
  benchmark::DoNotOptimize(size);
  auto* entry = &ArithmeticEntry;
  benchmark::DoNotOptimize(entry);
  const auto expected = kAotBenchmarkCases.front().expected;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = Call == CppEntryCall::Direct ? ArithmeticEntry(size) : entry(size);
    if (result != expected) {
      state.SkipWithError("C++ entry checksum mismatch");
      return;
    }
    benchmark::DoNotOptimize(result);
  }
  state.counters["result"] = static_cast<double>(expected);
  state.counters["expected_result"] = static_cast<double>(expected);
  ReportAotResident(state);
}

void JitSteady(benchmark::State& state,
               AotBenchmarkCase problem,
               const std::shared_ptr<JitWorkload>& workload) {
  runtime::NativeBindings bindings;
  {
    runtime::Isolate isolate;
    if (const auto registered = RegisterHost(isolate); !registered) {
      state.SkipWithError(registered.error());
      return;
    }
    bindings = isolate.FreezeBindings();
  }
  auto setup = LoadJitWorkload(*workload, std::move(bindings));
  if (!setup) {
    state.SkipWithError(setup.error());
    return;
  }
  auto& isolate = *setup->isolate;
  const auto run = [&]() -> std::expected<Sample, std::string> {
    const auto value = InvokeJitWorkload(*setup, problem.name);
    if (!value) {
      return std::unexpected("JIT benchmark execution failed");
    }
    return ResolveResult(isolate, *value, problem.completion);
  };
  const auto first_started = std::chrono::steady_clock::now();
  const auto warm = run();
  ReportJitFirstExecution(state, first_started);
  if (!warm || warm->value != problem.expected) {
    state.SkipWithError(warm ? "JIT warm-up checksum mismatch" : warm.error());
    return;
  }
  CollectHeapBaseline(state, isolate.heap());
  ReportSample(state, *warm);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = run();
    if (!result || result->value != problem.expected) {
      state.SkipWithError(result ? "JIT checksum mismatch" : result.error());
      return;
    }
    benchmark::DoNotOptimize(result->value);
  }
  ReportJitSetup(state, *setup);
  ReportJitMemory(state, *setup);
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
  if (scheduler.PumpUntil(result) == runtime::FuturePumpResult::Idle || completion_error) {
    return std::unexpected("C++ async benchmark became idle");
  }
  const auto value = result.value().value_or(runtime::Value::Null());
  if (!value.IsInteger()) {
    return std::unexpected("C++ async benchmark returned a non-int checksum");
  }
  return Sample{.value = value.AsInteger()};
}

std::int64_t CppSpectralNorm() {
  // 两套 Vela 用例使用相同的 500 阶、两次幂迭代；这里只转换原生用例的定点结果。
  constexpr double kFixedPointScale = 1'000'000'000.0;
  algorithms::CppWorkload workload(algorithms::Workload::SpectralNorm);
  return static_cast<std::int64_t>(workload.Run() * kFixedPointScale);
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
      sum = ArithmeticSum(size);
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

enum class EntryOperation : std::uint8_t { StackBounds, Execution, InstanceBorrow };

template <EntryOperation Operation>
void NativeEntryCost(benchmark::State& state) {
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
  auto& source = **instance;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    if constexpr (Operation == EntryOperation::StackBounds) {
      auto bounds = runtime::CurrentNativeStackBounds();
      benchmark::DoNotOptimize(bounds);
      if (!bounds) {
        state.SkipWithError("cannot query native stack bounds");
        return;
      }
    } else if constexpr (Operation == EntryOperation::Execution) {
      runtime::NativeExecution execution(source);
      benchmark::DoNotOptimize(execution);
    } else {
      auto borrowed = runtime::NativeInstance::Borrow(*source.state().front().AsObject());
      benchmark::DoNotOptimize(borrowed);
    }
  }
}

void JitEntryCost(benchmark::State& state, const std::shared_ptr<JitWorkload>& workload) {
  runtime::Isolate registrations;
  if (const auto registered = RegisterHost(registrations); !registered) {
    state.SkipWithError(registered.error());
    return;
  }
  auto setup = LoadJitWorkload(*workload, registrations.FreezeBindings());
  if (!setup) {
    state.SkipWithError(setup.error());
    return;
  }
  auto instance =
      runtime::NativeInstance::Borrow(setup->isolate->native_dispatch().current_state());
  const auto function = instance.FindFunction("arithmetic");
  if (!function) {
    state.SkipWithError("arithmetic entry is missing");
    return;
  }
  runtime::NativeExecution execution(instance);
  const auto& problem = kAotBenchmarkCases.front();
  const auto warm = runtime::InvokeNativeTarget(execution, *function, {});
  if (!warm || !warm->IsInteger() || warm->AsInteger() != problem.expected) {
    state.SkipWithError("arithmetic warm-up checksum mismatch");
    return;
  }
  // 诊断生成代码本身的入口成本；此版本及激活在整个测量期间保活。
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = runtime::InvokeNativeTarget(execution, *function, {});
    benchmark::DoNotOptimize(result);
    if (!result || !result->IsInteger() || result->AsInteger() != problem.expected) {
      state.SkipWithError("arithmetic checksum mismatch");
      return;
    }
  }
}

int RegisterAotBenchmarks() {
  benchmark::AddCustomContext("jit_steady_setup", "shared-image/fresh-isolate/v1");
  const auto baseline =
      std::make_shared<JitWorkload>(VELA_AOT_BENCHMARK_SOURCE, NativeCodeTier::Baseline);
  const auto optimized =
      std::make_shared<JitWorkload>(VELA_AOT_BENCHMARK_SOURCE, NativeCodeTier::Optimized);
  benchmark::AddCustomContext("aot_runtime_abi", std::string(runtime::NativeRuntimeAbiIdentity()));
  benchmark::AddCustomContext(
      "aot_codegen",
      "LLVM " +
          std::string(compiler::NativeOptimizationName(compiler::NativeOptimization::Release)) +
          "; generic CPU; PIC; checked arithmetic; explicit roots");
  benchmark::RegisterBenchmark("entry/stack_bounds", NativeEntryCost<EntryOperation::StackBounds>)
      ->UseRealTime();
  benchmark::RegisterBenchmark("entry/execution", NativeEntryCost<EntryOperation::Execution>)
      ->UseRealTime();
  benchmark::RegisterBenchmark("entry/instance_borrow",
                               NativeEntryCost<EntryOperation::InstanceBorrow>)
      ->UseRealTime();
  benchmark::RegisterBenchmark("entry/cpp_direct/arithmetic",
                               CppArithmeticEntry<CppEntryCall::Direct>)
      ->UseRealTime();
  benchmark::RegisterBenchmark("entry/cpp_indirect/arithmetic",
                               CppArithmeticEntry<CppEntryCall::Indirect>)
      ->UseRealTime();
  const std::string baseline_entry = "entry/baseline_arithmetic";
  const std::string optimized_entry = "entry/optimized_arithmetic";
  RegisterJitPreparation(baseline_entry, baseline);
  RegisterJitPreparation(optimized_entry, optimized);
  benchmark::RegisterBenchmark(baseline_entry, JitEntryCost, baseline)->UseRealTime();
  benchmark::RegisterBenchmark(optimized_entry, JitEntryCost, optimized)->UseRealTime();
  for (const auto& problem : kAotBenchmarkCases) {
    const auto suffix = std::string(problem.name) + "/" + std::to_string(problem.size);
    if (problem.kernel == AotKernel::Arithmetic || problem.kernel == AotKernel::Calls ||
        problem.kernel == AotKernel::Host) {
      benchmark::RegisterBenchmark("entry/generated/" + suffix,
                                   NativeEntryLayer<EntryCallLayer::Generated>, problem)
          ->UseRealTime();
      benchmark::RegisterBenchmark("entry/target/" + suffix,
                                   NativeEntryLayer<EntryCallLayer::Target>, problem)
          ->UseRealTime();
    }
    benchmark::RegisterBenchmark("aot/native/" + suffix, NativeSteady, problem)->UseRealTime();
    benchmark::RegisterBenchmark("aot/cpp/" + suffix, CppSteady, problem)->UseRealTime();
    benchmark::RegisterBenchmark("aot/first_native/" + suffix, NativeFirst, problem)->UseRealTime();
    const auto baseline_name = "hot/steady/baseline/" + suffix;
    const auto optimized_name = "hot/steady/optimized/" + suffix;
    RegisterJitPreparation(baseline_name, baseline);
    RegisterJitPreparation(optimized_name, optimized);
    benchmark::RegisterBenchmark(baseline_name, JitSteady, problem, baseline)->UseRealTime();
    benchmark::RegisterBenchmark(optimized_name, JitSteady, problem, optimized)->UseRealTime();
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = RegisterAotBenchmarks();

}  // namespace
}  // namespace vela::benchmarks
