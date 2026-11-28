#include "benchmarks/production/aot.h"

#include <array>
#include <cstddef>
#include <expected>

#include <benchmark/benchmark.h>

#include "base/execution_status.h"
#include "benchmarks/process_memory.h"
#include "benchmarks/production/contract.h"
#include "benchmarks/production/measurement.h"
#include "benchmarks/production_native/entries.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

namespace vela::benchmarks::production {

static std::expected<runtime::Value, ExecutionStatus> Invoke(runtime::Isolate& isolate,
                                                             runtime::NativeExecution& execution,
                                                             const Case& problem) {
  auto value = runtime::InvokeNativeTarget(execution, problem.function, {});
  if (!value || problem.family != Family::Async) {
    return value;
  }
  runtime::RootRange root(isolate.heap(), &*value, 1);
  auto* future = value->IsObject() ? runtime::Heap::GetFuture(*value->AsObject()) : nullptr;
  if (future == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& scheduler = isolate.scheduler();
  if (scheduler.PumpUntil(*future) == runtime::FuturePumpResult::Idle ||
      future->state() != runtime::FutureState::Succeeded) {
    return std::unexpected(ExecutionStatus::NativeFailure);
  }
  return future->result().value;
}

static void ReportAotAllocations(benchmark::State& state,
                                 runtime::Isolate& isolate,
                                 runtime::NativeExecution& execution,
                                 const Case& problem,
                                 std::size_t baseline,
                                 std::size_t empty_baseline) {
  auto& heap = isolate.heap();
  const auto statistics = heap.allocation_statistics();
  const auto count = static_cast<double>(state.iterations());
  state.counters["allocations_per_invocation"] = static_cast<double>(statistics.count) / count;
  state.counters["allocation_bytes_per_invocation"] = static_cast<double>(statistics.bytes) / count;
  state.counters["peak_live_growth_bytes"] =
      static_cast<double>(statistics.peak_live_bytes - baseline);
  state.counters["baseline_bytes"] = static_cast<double>(baseline);
  heap.FinishCollection();
  heap.Collect();
  if (problem.family == Family::Churn) {
    const auto steady = heap.allocated_bytes();
    state.counters["steady_live_bytes"] = static_cast<double>(steady);
    constexpr std::size_t kRingTextGrowthLimit = kRingCapacity * kRingCapacity;
    if (steady > baseline + kRingTextGrowthLimit) {
      state.SkipWithError("AOT ring memory exceeded its bounded plateau");
    }
    if (!runtime::InvokeNativeTarget(execution, problem.cleanup, {})) {
      state.SkipWithError("AOT cleanup failed");
      return;
    }
    heap.FinishCollection();
    heap.Collect();
  }
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes());
  const auto expected_retained = problem.family == Family::Churn ? empty_baseline : baseline;
  state.counters["expected_retained_bytes"] = static_cast<double>(expected_retained);
  if (heap.allocated_bytes() != expected_retained) {
    state.SkipWithError("AOT workload retained temporary values");
  }
  auto& scheduler = isolate.scheduler();
  state.counters["remaining_suspended_frames"] =
      static_cast<double>(scheduler.suspended_frame_count());
  state.counters["remaining_saved_bytes"] = static_cast<double>(scheduler.saved_state_bytes());
  if (scheduler.suspended_frame_count() != 0 || scheduler.saved_state_bytes() != 0) {
    state.SkipWithError("AOT completed async work retained suspended state");
  }
}

template <Measurement Mode>
static void MeasureAot(benchmark::State& state, const Case& problem, const Options& options) {
  runtime::Isolate isolate;
  const auto instance = runtime::NativeInstance::Instantiate(isolate, *kImage);
  if (!instance) {
    state.SkipWithError(instance.error().message);
    return;
  }
  runtime::NativeExecution execution(**instance);
  const std::array input{
      runtime::EncodeNativeValue(runtime::Value::Integer(problem.size)),
      runtime::EncodeNativeValue(runtime::Value::Integer(kSuccess)),
  };
  if (!runtime::InvokeNativeTarget(execution, problem.prepare, input)) {
    state.SkipWithError("AOT input preparation failed");
    return;
  }
  auto& heap = isolate.heap();
  heap.FinishCollection();
  heap.Collect();
  const auto empty_baseline = heap.allocated_bytes();
  const auto warmups = problem.family == Family::Churn ? kRingCapacity : std::size_t{1};
  for (std::size_t warmup = 0; warmup < warmups; ++warmup) {
    const auto warm = Invoke(isolate, execution, problem);
    if (!warm || !warm->IsInteger() || warm->AsInteger() != problem.expected) {
      state.SkipWithError("AOT warm-up checksum mismatch");
      return;
    }
  }
  heap.FinishCollection();
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  heap.ResetAllocationStatistics();
  LatencySamples samples(Mode == Measurement::Latency ? options.samples : 0);
  const auto resident = CurrentResidentBytes();
  if (!resident) {
    state.SkipWithError(resident.error());
    return;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    benchmark::ClobberMemory();
    [[maybe_unused]] LatencySamples::Clock::time_point started;
    if constexpr (Mode == Measurement::Latency) {
      started = LatencySamples::Clock::now();
    }
    auto value = Invoke(isolate, execution, problem);
    if (!value || !value->IsInteger() || value->AsInteger() != problem.expected) {
      state.SkipWithError("AOT checksum mismatch");
      return;
    }
    auto result = value->AsInteger();
    benchmark::DoNotOptimize(result);
    if constexpr (Mode == Measurement::Latency) {
      samples.Record(state, started);
    }
  }
  if constexpr (Mode == Measurement::Latency) {
    samples.Report(state);
    ReportResidentMemory(state, *resident);
  }
  if constexpr (Mode == Measurement::Allocations) {
    ReportAotAllocations(state, isolate, execution, problem, baseline, empty_baseline);
  }
  state.counters["result"] = static_cast<double>(problem.expected);
  state.counters["expected_result"] = static_cast<double>(problem.expected);
}

void RunAot(benchmark::State& state, const Case& problem, const Options& options) {
  switch (options.measurement) {
    case Measurement::Throughput: {
      MeasureAot<Measurement::Throughput>(state, problem, options);
      return;
    }
    case Measurement::Latency: {
      MeasureAot<Measurement::Latency>(state, problem, options);
      return;
    }
    case Measurement::Allocations: {
      MeasureAot<Measurement::Allocations>(state, problem, options);
      return;
    }
  }
}

}  // namespace vela::benchmarks::production
