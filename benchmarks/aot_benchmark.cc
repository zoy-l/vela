#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "benchmarks/algorithms/workloads.h"
#include "benchmarks/aot_benchmark_kernels.h"
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
  return kAotWorkloadSize;
}

[[clang::noinline]] std::int64_t HostStep([[maybe_unused]] runtime::NativeContext& context,
                                          std::int64_t value) noexcept {
  return value + 1;
}

std::expected<void, std::string> RegisterHost(runtime::Isolate& isolate) {
  if (!isolate.RegisterNative(runtime::NativeFunction::Bind<WorkloadSize>("benchmark.size")) ||
      !isolate.RegisterNative(runtime::NativeFunction::Bind<HostStep>("benchmark.step"))) {
    return std::unexpected("benchmark host registration failed");
  }
  return {};
}

std::expected<void, ExecutionStatus> PrepareNative(runtime::NativeExecution& execution,
                                                   AotBenchmarkCase problem) {
  if (problem.kernel != AotKernel::Arithmetic && problem.kernel != AotKernel::Calls) {
    return {};
  }
  const std::array arguments{
      runtime::EncodeNativeValue(runtime::Value::Integer(problem.size)),
      runtime::EncodeNativeValue(runtime::Value::Integer(kAotInputSeed)),
  };
  const auto result = runtime::InvokeNativeTarget(execution, kAotPrepareNumeric, arguments);
  if (!result) {
    return std::unexpected(result.error());
  }
  return {};
}

std::expected<runtime::Value, ExecutionStatus> AwaitResult(runtime::Isolate& isolate,
                                                           runtime::Value value) {
  runtime::RootRange root(isolate.heap(), &value, 1);
  auto* future = value.IsObject() ? runtime::Heap::GetFuture(*value.AsObject()) : nullptr;
  if (future == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& scheduler = isolate.scheduler();
  if (scheduler.PumpUntil(*future) == runtime::FuturePumpResult::Idle ||
      future->state() != runtime::FutureState::Succeeded) {
    return std::unexpected(ExecutionStatus::NativeFailure);
  }
  return future->value().value_or(runtime::Value::Null());
}

template <AotCompletion Completion>
std::expected<runtime::Value, ExecutionStatus> CompleteResult(
    runtime::Isolate& isolate,
    std::expected<runtime::Value, ExecutionStatus> value) {
  if constexpr (Completion == AotCompletion::Future) {
    if (value) {
      return AwaitResult(isolate, *value);
    }
  }
  return value;
}

void ReportPendingState(benchmark::State& state, runtime::Isolate& isolate) {
  auto& scheduler = isolate.scheduler();
  state.counters["suspended_frames"] = static_cast<double>(scheduler.suspended_frame_count());
  state.counters["saved_state_bytes"] = static_cast<double>(scheduler.saved_state_bytes());
}

template <AotCompletion Completion>
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
  if (!PrepareNative(execution, problem)) {
    state.SkipWithError("native input preparation failed");
    return;
  }
  const auto invoked = runtime::InvokeNativeTarget(execution, problem.function, {});
  ReportPendingState(state, isolate);
  const auto warm = CompleteResult<Completion>(isolate, invoked);
  if (!warm || !warm->IsInteger() || warm->AsInteger() != problem.expected) {
    state.SkipWithError("native warm-up checksum mismatch");
    return;
  }
  CollectHeapBaseline(state, isolate.heap());
  state.counters["result"] = static_cast<double>(problem.expected);
  state.counters["expected_result"] = static_cast<double>(problem.expected);
  state.counters["entry_shadow_slots"] = static_cast<double>(problem.shadow_slots);
  state.counters["entry_frame_bytes"] = static_cast<double>(problem.frame_bytes);
  state.counters["entry_static_root_stores"] = static_cast<double>(problem.root_stores);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    benchmark::ClobberMemory();
    const auto result = CompleteResult<Completion>(
        isolate, runtime::InvokeNativeTarget(execution, problem.function, {}));
    if (!result || !result->IsInteger() || result->AsInteger() != problem.expected) {
      state.SkipWithError("native checksum mismatch");
      return;
    }
    auto value = result->AsInteger();
    benchmark::DoNotOptimize(value);
  }
  ReportAotHeap(state, isolate);
}

enum class EntryCallLayer : std::uint8_t { Generated, Target };

template <EntryCallLayer Layer>
void NativeEntryLayer(benchmark::State& state) {
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
  auto input = kEntryInput;
  std::array arguments{runtime::EncodeNativeValue(runtime::Value::Integer(input))};
  const auto warm = runtime::InvokeNativeTarget(execution, kAotScalarEntry, arguments);
  if (!warm || !warm->IsInteger() || warm->AsInteger() != kEntryExpected) {
    state.SkipWithError("entry warm-up checksum mismatch");
    return;
  }
  auto entry = (*instance)->image().function(kAotScalarEntry).entry;
  benchmark::DoNotOptimize(entry);
  CollectHeapBaseline(state, isolate.heap());
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    benchmark::DoNotOptimize(input);
    arguments.front() = runtime::EncodeNativeValue(runtime::Value::Integer(input));
    runtime::Value value;
    if constexpr (Layer == EntryCallLayer::Generated) {
      runtime::NativeValue output;
      const auto status = entry(&execution, arguments.data(), arguments.size(), &output);
      if (status != runtime::ToNativeStatus(ExecutionStatus::Returned)) {
        state.SkipWithError("generated entry failed");
        return;
      }
      value = runtime::DecodeNativeValue(output);
    } else {
      const auto result = runtime::InvokeNativeTarget(execution, kAotScalarEntry, arguments);
      if (!result) {
        state.SkipWithError("native target failed");
        return;
      }
      value = *result;
    }
    if (!value.IsInteger() || value.AsInteger() != kEntryExpected) {
      state.SkipWithError("entry checksum mismatch");
      return;
    }
    auto result = value.AsInteger();
    benchmark::DoNotOptimize(result);
  }
  state.counters["result"] = static_cast<double>(kEntryExpected);
  state.counters["expected_result"] = static_cast<double>(kEntryExpected);
  ReportAotHeap(state, isolate);
}

enum class CppEntryCall : std::uint8_t { Direct, Indirect };

template <CppEntryCall Call>
void CppEntry(benchmark::State& state) {
  auto input = kEntryInput;
  auto* entry = &CppScalarEntry;
  benchmark::DoNotOptimize(entry);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    benchmark::DoNotOptimize(input);
    auto result = Call == CppEntryCall::Direct ? CppScalarEntry(input) : entry(input);
    if (result != kEntryExpected) {
      state.SkipWithError("C++ entry checksum mismatch");
      return;
    }
    benchmark::DoNotOptimize(result);
  }
  state.counters["result"] = static_cast<double>(kEntryExpected);
  state.counters["expected_result"] = static_cast<double>(kEntryExpected);
  ReportAotResident(state);
}

template <AotCompletion Completion>
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
  if (problem.kernel == AotKernel::Arithmetic || problem.kernel == AotKernel::Calls) {
    const auto entry = isolate.native_dispatch().FindEntry(setup->program->entry, "prepareNumeric");
    const std::array arguments{
        runtime::Value::Integer(problem.size),
        runtime::Value::Integer(kAotInputSeed),
    };
    if (!entry || !entry->Invoke(arguments)) {
      state.SkipWithError("JIT input preparation failed");
      return;
    }
  }
  const auto first_started = std::chrono::steady_clock::now();
  const auto invoked = InvokeJitWorkload(*setup, problem.name);
  auto& scheduler = isolate.scheduler();
  const auto suspended_frames = scheduler.suspended_frame_count();
  const auto saved_bytes = scheduler.saved_state_bytes();
  const auto warm = CompleteResult<Completion>(isolate, invoked);
  ReportJitFirstExecution(state, first_started);
  if (!warm || !warm->IsInteger() || warm->AsInteger() != problem.expected) {
    state.SkipWithError("JIT warm-up checksum mismatch");
    return;
  }
  CollectHeapBaseline(state, isolate.heap());
  state.counters["result"] = static_cast<double>(problem.expected);
  state.counters["expected_result"] = static_cast<double>(problem.expected);
  state.counters["suspended_frames"] = static_cast<double>(suspended_frames);
  state.counters["saved_state_bytes"] = static_cast<double>(saved_bytes);
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    benchmark::ClobberMemory();
    const auto result =
        CompleteResult<Completion>(isolate, InvokeJitWorkload(*setup, problem.name));
    if (!result || !result->IsInteger() || result->AsInteger() != problem.expected) {
      state.SkipWithError("JIT checksum mismatch");
      return;
    }
    auto value = result->AsInteger();
    benchmark::DoNotOptimize(value);
  }
  ReportJitSetup(state, *setup);
  ReportJitMemory(state, *setup);
}

std::expected<std::int64_t, ExecutionStatus> CppAsync(runtime::Isolate& isolate,
                                                      std::int64_t size) {
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
    return std::unexpected(ExecutionStatus::NativeFailure);
  }
  const auto value = result.value().value_or(runtime::Value::Null());
  if (!value.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return value.AsInteger();
}

std::int64_t CppSpectralNorm() {
  // 两套 Vela 用例使用相同的 500 阶、两次幂迭代；这里只转换原生用例的定点结果。
  constexpr double kFixedPointScale = 1'000'000'000.0;
  algorithms::CppWorkload workload(algorithms::Workload::SpectralNorm);
  return static_cast<std::int64_t>(workload.Run() * kFixedPointScale);
}

std::int64_t CppRun(runtime::Isolate& isolate,
                    AotBenchmarkCase problem,
                    std::span<const std::int64_t> numeric_values) {
  auto size = problem.size;
  benchmark::DoNotOptimize(size);
  std::int64_t sum = 0;
  struct Box final {
    std::int64_t value = 0;
    std::string text;
  };
  switch (problem.kernel) {
    case AotKernel::Arithmetic:
      sum = CppArithmetic(numeric_values);
      break;
    case AotKernel::Calls:
      sum = CppCalls(numeric_values);
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
      std::unreachable();
    case AotKernel::SpectralNorm:
      return CppSpectralNorm();
  }
  return sum;
}

template <AotCompletion Completion>
void CppSteady(benchmark::State& state, AotBenchmarkCase problem) {
  runtime::Isolate isolate;
  const auto numeric_values =
      problem.kernel == AotKernel::Arithmetic || problem.kernel == AotKernel::Calls
          ? PrepareNumericValues({.size = problem.size, .seed = kAotInputSeed})
          : std::vector<std::int64_t>{};
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    benchmark::ClobberMemory();
    if constexpr (Completion == AotCompletion::Future) {
      auto result = CppAsync(isolate, problem.size);
      if (!result || *result != problem.expected) {
        state.SkipWithError("C++ async benchmark checksum mismatch");
        return;
      }
      benchmark::DoNotOptimize(*result);
    } else {
      auto result = CppRun(isolate, problem, numeric_values);
      if (result != problem.expected) {
        state.SkipWithError("C++ benchmark checksum mismatch");
        return;
      }
      benchmark::DoNotOptimize(result);
    }
  }
  state.counters["result"] = static_cast<double>(problem.expected);
  state.counters["expected_result"] = static_cast<double>(problem.expected);
  ReportAotResident(state);
}

template <AotCompletion Completion>
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
      if (!PrepareNative(execution, problem)) {
        state.ResumeTiming();
        state.SkipWithError("native input preparation failed");
        return;
      }
      state.ResumeTiming();
      const auto result = CompleteResult<Completion>(
          isolate, runtime::InvokeNativeTarget(execution, problem.function, {}));
      state.PauseTiming();
      if (!result || !result->IsInteger() || result->AsInteger() != problem.expected) {
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
  const auto function = instance.FindFunction("entryScalar");
  if (!function) {
    state.SkipWithError("scalar entry is missing");
    return;
  }
  runtime::NativeExecution execution(instance);
  auto input = kEntryInput;
  std::array arguments{runtime::EncodeNativeValue(runtime::Value::Integer(input))};
  const auto warm = runtime::InvokeNativeTarget(execution, *function, arguments);
  if (!warm || !warm->IsInteger() || warm->AsInteger() != kEntryExpected) {
    state.SkipWithError("scalar warm-up checksum mismatch");
    return;
  }
  // 诊断生成代码本身的入口成本；此版本及激活在整个测量期间保活。
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    benchmark::DoNotOptimize(input);
    arguments.front() = runtime::EncodeNativeValue(runtime::Value::Integer(input));
    const auto result = runtime::InvokeNativeTarget(execution, *function, arguments);
    if (!result || !result->IsInteger() || result->AsInteger() != kEntryExpected) {
      state.SkipWithError("scalar checksum mismatch");
      return;
    }
    auto value = result->AsInteger();
    benchmark::DoNotOptimize(value);
  }
  state.counters["result"] = static_cast<double>(kEntryExpected);
  state.counters["expected_result"] = static_cast<double>(kEntryExpected);
}

template <AotCompletion Completion>
void RegisterWorkload(AotBenchmarkCase problem,
                      const std::array<std::shared_ptr<JitWorkload>, 2>& tiers) {
  const auto suffix = std::string(problem.name) + "/" + std::to_string(problem.size);
  benchmark::RegisterBenchmark("aot/native/" + suffix, NativeSteady<Completion>, problem)
      ->UseRealTime();
  benchmark::RegisterBenchmark("aot/cpp/" + suffix, CppSteady<Completion>, problem)->UseRealTime();
  benchmark::RegisterBenchmark("aot/first_native/" + suffix, NativeFirst<Completion>, problem)
      ->UseRealTime();
  constexpr std::array kTierNames{"baseline", "optimized"};
  for (std::size_t index = 0; index < tiers.size(); ++index) {
    const auto name = "hot/steady/" + std::string(kTierNames[index]) + "/" + suffix;
    const auto& workload = tiers[index];
    RegisterJitPreparation(name, workload);
    benchmark::RegisterBenchmark(name, JitSteady<Completion>, problem, workload)->UseRealTime();
  }
}

int RegisterAotBenchmarks() {
  benchmark::AddCustomContext("jit_steady_setup", "shared-image/fresh-isolate/v1");
  const std::array tiers{
      std::make_shared<JitWorkload>(VELA_AOT_BENCHMARK_SOURCE, NativeCodeTier::Baseline),
      std::make_shared<JitWorkload>(VELA_AOT_BENCHMARK_SOURCE, NativeCodeTier::Optimized),
  };
  benchmark::AddCustomContext("aot_runtime_abi", std::string(runtime::NativeRuntimeAbiIdentity()));
  benchmark::AddCustomContext(
      "aot_codegen",
      "LLVM " +
          std::string(compiler::NativeOptimizationName(compiler::NativeOptimization::Release)) +
          "; generic CPU; PIC; checked arithmetic; explicit roots");
  benchmark::AddCustomContext("aot_workload_contract", "runtime-inputs/specialized-completion/v2");
  benchmark::RegisterBenchmark("entry/stack_bounds", NativeEntryCost<EntryOperation::StackBounds>)
      ->UseRealTime();
  benchmark::RegisterBenchmark("entry/execution", NativeEntryCost<EntryOperation::Execution>)
      ->UseRealTime();
  benchmark::RegisterBenchmark("entry/instance_borrow",
                               NativeEntryCost<EntryOperation::InstanceBorrow>)
      ->UseRealTime();
  benchmark::RegisterBenchmark("entry/cpp_direct/scalar", CppEntry<CppEntryCall::Direct>)
      ->UseRealTime();
  benchmark::RegisterBenchmark("entry/cpp_indirect/scalar", CppEntry<CppEntryCall::Indirect>)
      ->UseRealTime();
  benchmark::RegisterBenchmark("entry/generated/scalar",
                               NativeEntryLayer<EntryCallLayer::Generated>)
      ->UseRealTime();
  benchmark::RegisterBenchmark("entry/target/scalar", NativeEntryLayer<EntryCallLayer::Target>)
      ->UseRealTime();
  const std::string baseline_entry = "entry/baseline_scalar";
  const std::string optimized_entry = "entry/optimized_scalar";
  RegisterJitPreparation(baseline_entry, tiers.front());
  RegisterJitPreparation(optimized_entry, tiers.back());
  benchmark::RegisterBenchmark(baseline_entry, JitEntryCost, tiers.front())->UseRealTime();
  benchmark::RegisterBenchmark(optimized_entry, JitEntryCost, tiers.back())->UseRealTime();
  for (const auto& problem : kAotBenchmarkCases) {
    if (problem.completion == AotCompletion::Future) {
      RegisterWorkload<AotCompletion::Future>(problem, tiers);
    } else {
      RegisterWorkload<AotCompletion::Immediate>(problem, tiers);
    }
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = RegisterAotBenchmarks();

}  // namespace
}  // namespace vela::benchmarks
