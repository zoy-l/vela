#include <cstdint>
#include <string>
#include <string_view>

#include "benchmark/benchmark.h"

#include "benchmarks/source.h"
#include "runtime/future.h"
#include "runtime/isolate.h"
#include "runtime/value.h"
#include "vm/vm.h"

namespace vela::benchmarks {
namespace {

constexpr std::int64_t kAnswer = 42;
constexpr std::string_view kNoAwait = R"(
async func task(): Future<int> { return 42; }
func main(): Future<int> { return task(); }
)";
constexpr std::string_view kCompletedAwait = R"(
async func main(): Future<int> { return await Future.value(42); }
)";
constexpr std::string_view kSuspendResume = R"(
async func main(): Future<int> { return await Future.microtask(() => 42); }
)";
constexpr std::string_view kNoAwaitLoop = R"(
async func task(): Future<int> { return 42; }
async func main(): Future<int> {
  for (let index = 0; index < 1000; index++) { task(); }
  return 42;
}
)";

enum class Collection : std::uint8_t { Automatic, WhileSuspended };

bool IsAnswer(const runtime::Future& result, std::int64_t expected) {
  const auto value = result.value();
  return result.state() == runtime::FutureState::Succeeded && value && value->IsInteger() &&
         value->AsInteger() == expected;
}

void SteadyExecution(benchmark::State& state,
                     std::string_view source,
                     std::int64_t expected,
                     Collection collection) {
  runtime::Isolate isolate;
  const auto entry = CompileBenchmarkEntry(state, source, isolate);
  if (!entry) {
    return;
  }
  const auto& program = entry->program();
  const auto entry_index = entry->index();
  vm::VirtualMachine machine(isolate);
  auto& scheduler = isolate.scheduler();
  auto& heap = isolate.heap();
  const auto before = heap.allocated_bytes();
  {
    auto result = machine.ExecuteFuture(program, entry_index);
    const auto frames = scheduler.suspended_frame_count();
    const auto saved = scheduler.saved_state_bytes();
    state.counters["suspended_frames"] = static_cast<double>(frames);
    state.counters["saved_state_bytes"] = static_cast<double>(saved);
    state.counters["saved_bytes_per_frame"] =
        frames == 0 ? 0.0 : static_cast<double>(saved) / static_cast<double>(frames);
    state.counters["first_async_bytes"] = static_cast<double>(heap.allocated_bytes() - before);
    if (!scheduler.PumpUntil(result) || !IsAnswer(result, expected)) {
      state.SkipWithError("Async warm-up failed or returned an unexpected value");
      return;
    }
  }
  state.counters["register_bytes"] = static_cast<double>(machine.register_bytes());
  heap.FinishCollection();
  heap.Collect();
  const auto retained = heap.allocated_bytes();
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto result = machine.ExecuteFuture(program, entry_index);
    if (collection == Collection::WhileSuspended) {
      heap.FinishCollection();
      heap.Collect();
    }
    if (!scheduler.PumpUntil(result) || !IsAnswer(result, expected)) {
      state.SkipWithError("Async execution failed or returned an unexpected value");
      return;
    }
    benchmark::DoNotOptimize(result);
  }
  heap.FinishCollection();
  heap.Collect();
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes());
  if (scheduler.suspended_frame_count() != 0 || heap.allocated_bytes() != retained) {
    state.SkipWithError("Completed async tasks retained suspended frames or heap values");
  }
}

void FirstExecution(benchmark::State& state) {
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    state.PauseTiming();
    {
      runtime::Isolate isolate;
      const auto entry = CompileBenchmarkEntry(state, kNoAwait, isolate);
      if (!entry) {
        return;
      }
      const auto& program = entry->program();
      const auto entry_index = entry->index();
      vm::VirtualMachine machine(isolate);
      state.ResumeTiming();
      auto result = machine.ExecuteFuture(program, entry_index);
      benchmark::DoNotOptimize(result);
      state.PauseTiming();
      if (!IsAnswer(result, kAnswer)) {
        state.SkipWithError("First async call failed or returned an unexpected value");
        return;
      }
    }
    state.ResumeTiming();
  }
}

std::string ConcurrentSource(std::int64_t count) {
  return R"(
async func task(input: Future<int>): Future<int> {
  const live = [40, 2];
  const value = await input;
  return value + live[0] + live[1];
}
async func main(): Future<int> {
  const producer = Completer<int>();
  let tasks: Future<int>[] = [];
  for (let index = 0; index < )" +
         std::to_string(count) + R"(; index++) {
    tasks.push(task(producer.future));
  }
  Timer.after(0, (): void => { producer.complete(0); });
  const values = await Future.wait(tasks);
  let sum = 0;
  for (const value in values) { sum += value; }
  return sum;
}
)";
}

void ConcurrentTasks(benchmark::State& state, Collection collection) {
  const auto count = state.range(0);
  SteadyExecution(state, ConcurrentSource(count), count * kAnswer, collection);
}

void TimerScale(benchmark::State& state) {
  const auto count = state.range(0);
  const auto source = R"(
async func main(): Future<int> {
  let total = 0;
  for (let index = 0; index < )" +
                      std::to_string(count) + R"(; index++) {
    Timer.after(0, (): void => { total++; });
  }
  await sleep(0);
  return total;
}
)";
  SteadyExecution(state, source, count, Collection::Automatic);
}

BENCHMARK_CAPTURE(SteadyExecution, NoAwait, kNoAwait, kAnswer, Collection::Automatic)
    ->Name("async/no_await");
BENCHMARK_CAPTURE(SteadyExecution, CompletedAwait, kCompletedAwait, kAnswer, Collection::Automatic)
    ->Name("async/completed_await");
BENCHMARK_CAPTURE(SteadyExecution, SuspendResume, kSuspendResume, kAnswer, Collection::Automatic)
    ->Name("async/suspend_resume");
BENCHMARK_CAPTURE(SteadyExecution, NoAwaitLoop, kNoAwaitLoop, kAnswer, Collection::Automatic)
    ->Name("async/no_await_loop");
BENCHMARK(FirstExecution)->Name("async/first_call")->Iterations(1000)->UseRealTime();
BENCHMARK_CAPTURE(ConcurrentTasks, Automatic, Collection::Automatic)
    ->Name("async/concurrent")
    ->Arg(1)
    ->Arg(100)
    ->Arg(1000);
BENCHMARK_CAPTURE(ConcurrentTasks, WhileSuspended, Collection::WhileSuspended)
    ->Name("async/suspended_gc")
    ->Arg(1)
    ->Arg(100)
    ->Arg(1000);
BENCHMARK(TimerScale)->Name("async/timers")->Arg(1)->Arg(100)->Arg(1000);

}  // namespace
}  // namespace vela::benchmarks
