#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "benchmark/benchmark.h"

#include "base/file.h"
#include "base/path.h"
#include "benchmarks/process_memory.h"
#include "compiler/compilation_cache.h"
#include "compiler/compiler.h"
#include "compiler/testing/result.h"
#include "compiler/testing/suite.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "vm/bytecode_image.h"
#include "vm/program.h"

namespace vela::benchmarks {
namespace {

constexpr std::string_view kSmall = R"(
import 'sdk:test';
func main(): void { test('small', () => { expect(42, 42); }); }
)";
constexpr std::string_view kRegistration = R"(
import 'sdk:test';
func main(): void {
  for (let index = 0; index < 1000; index++) { test('registered', () => {}); }
}
)";
constexpr std::string_view kAssertions = R"(
import 'sdk:test';
func main(): void {
  test('assertions', () => { for (let index = 0; index < 1000; index++) { expect(index, index); } });
}
)";
constexpr std::string_view kDeep = R"(
import 'sdk:test';
func main(): void {
  test('deep', () => {
    let left: dynamic = 42;
    let right: dynamic = 42;
    for (let index = 0; index < 2048; index++) { left = [left]; right = [right]; }
    expect(left, equals(right));
  });
}
)";
constexpr std::string_view kOverlap = R"(
import 'sdk:test';
class Overlap implements Matcher {
  bound: int;
  match(actual: dynamic): MatchResult {
    return MatchResult(success: actual is int && actual <= this.bound, description: 'bounded');
  }
}
func main(): void {
  test('overlap', () => {
    expect([1, 2, 3], unorderedEquals([Overlap(bound: 3), Overlap(bound: 2), Overlap(bound: 1)]));
  });
}
)";

bool IsSuccessful(const testing::SuiteResult& result, std::size_t expected) {
  return result.errors.empty() && result.exit_code == 0 && result.tests.size() == expected &&
         std::ranges::all_of(result.tests, [](const auto& test) {
           return test.status == testing::TestStatus::Passed;
         });
}

testing::SuiteResult RunSuite(runtime::Isolate& isolate, const vm::Program& program) {
  std::ostringstream diagnostics;
  testing::Suite suite(isolate, program, {}, diagnostics);
  return suite.Run();
}

void SmallStartup(benchmark::State& state) {
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    runtime::Isolate isolate;
    const auto compiled = compiler::CompileSource(kSmall, isolate);
    if (!compiled.Succeeded() || !IsSuccessful(RunSuite(isolate, compiled.program), 1)) {
      state.SkipWithError("small suite compilation or execution failed");
      return;
    }
  }
}

void SuiteExecution(benchmark::State& state, std::string_view source, std::size_t expected) {
  runtime::Isolate isolate;
  const auto compiled = compiler::CompileSource(source, isolate);
  if (!compiled.Succeeded()) {
    state.SkipWithError("cannot compile suite workload");
    return;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    if (!IsSuccessful(RunSuite(isolate, compiled.program), expected)) {
      state.SkipWithError("suite returned an unexpected result");
      return;
    }
  }
  state.counters["allocated_bytes"] = static_cast<double>(isolate.heap().allocated_bytes());
  state.counters["committed_bytes"] = static_cast<double>(isolate.heap().committed_bytes());
  isolate.heap().FinishCollection();
  isolate.heap().Collect();
  state.counters["retained_bytes"] = static_cast<double>(isolate.heap().allocated_bytes());
  state.counters["remaining_frames"] =
      static_cast<double>(isolate.scheduler().suspended_frame_count());
  if (isolate.scheduler().suspended_frame_count() != 0) {
    state.SkipWithError("completed suite retained suspended frames");
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(expected));
}

std::expected<std::vector<std::string>, std::string> PopulateSuiteCache(
    const std::filesystem::path& directory,
    std::size_t count) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return std::unexpected(error.message());
  }
  const auto source =
      ReadBinaryFile(std::filesystem::path(VELA_BENCHMARK_SOURCE_DIR) / "testing_workload.vela");
  if (!source) {
    return std::unexpected(source.error());
  }
  std::vector<std::string> paths;
  paths.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto path = directory / (std::to_string(index) + "_test.vela");
    const auto written = WriteFileAtomically(path, *source);
    if (!written) {
      return std::unexpected(written.error());
    }
    paths.push_back(PathText(path));
    runtime::Isolate isolate;
    const auto populated = compiler::CompileFileCached(paths.back(), directory / "cache", isolate);
    if (!populated) {
      return std::unexpected(populated.error());
    }
    if (!populated->compiled.Succeeded()) {
      return std::unexpected("cannot compile SDK cache inputs");
    }
  }
  return paths;
}

void CacheBatch(benchmark::State& state) {
  std::error_code error;
  const auto temporary = std::filesystem::temp_directory_path(error);
  if (error) {
    state.SkipWithError(error.message());
    return;
  }
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto cache = temporary / ("vela-testing-benchmark-" + std::to_string(suffix));
  const auto paths = PopulateSuiteCache(cache, static_cast<std::size_t>(state.range(0)));
  if (!paths) {
    state.SkipWithError(paths.error());
    std::filesystem::remove_all(cache, error);
    return;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    for (const auto& path : *paths) {
      runtime::Isolate isolate;
      const auto compiled = compiler::CompileFileCached(path, cache / "cache", isolate);
      if (!compiled || compiled->status != compiler::CacheStatus::Hit ||
          !IsSuccessful(RunSuite(isolate, compiled->compiled.program), 1)) {
        state.SkipWithError("cached suite failed or missed its cache");
        break;
      }
    }
    if (state.skipped()) {
      break;
    }
  }
  std::filesystem::remove_all(cache, error);
  if (error) {
    state.SkipWithError(error.message());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

void ConcurrentSuites(benchmark::State& state) {
  runtime::Isolate producer;
  const auto compiled = compiler::CompileSource(kDeep, producer);
  if (!compiled.Succeeded()) {
    state.SkipWithError("cannot compile concurrent suite");
    return;
  }
  const auto image = vm::SaveBytecode(compiled.program, producer);
  if (!image) {
    state.SkipWithError(image.error().message);
    return;
  }
  const auto count = static_cast<std::size_t>(state.range(0));
  std::vector<std::size_t> allocated(count);
  std::vector<std::size_t> committed(count);
  std::atomic_bool failed{false};
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    std::barrier loaded(static_cast<std::ptrdiff_t>(count));
    std::barrier finished(static_cast<std::ptrdiff_t>(count));
    std::vector<std::jthread> workers;
    workers.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      workers.emplace_back([&, index] {
        runtime::Isolate isolate;
        const auto program = vm::LoadBytecode(*image, isolate);
        loaded.arrive_and_wait();
        if (!program || !IsSuccessful(RunSuite(isolate, *program), 1)) {
          failed = true;
        }
        allocated.at(index) = isolate.heap().allocated_bytes();
        committed.at(index) = isolate.heap().committed_bytes();
        finished.arrive_and_wait();
      });
    }
    workers.clear();
    if (failed) {
      state.SkipWithError("concurrent suite failed");
      return;
    }
  }
  state.counters["combined_allocated_bytes"] =
      static_cast<double>(std::accumulate(allocated.begin(), allocated.end(), std::size_t{0}));
  state.counters["combined_committed_bytes"] =
      static_cast<double>(std::accumulate(committed.begin(), committed.end(), std::size_t{0}));
  const auto peak = PeakResidentBytes();
  if (!peak) {
    state.SkipWithError(peak.error());
    return;
  }
  state.counters["process_peak_bytes"] = static_cast<double>(*peak);
}

BENCHMARK(SmallStartup)->Name("testing/small_startup")->UseRealTime();
BENCHMARK_CAPTURE(SuiteExecution, Registration, kRegistration, 1000)
    ->Name("testing/registration_1000")
    ->UseRealTime();
BENCHMARK_CAPTURE(SuiteExecution, Assertions, kAssertions, 1)
    ->Name("testing/assertions_1000")
    ->UseRealTime();
BENCHMARK_CAPTURE(SuiteExecution, Deep, kDeep, 1)->Name("testing/deep_2048")->UseRealTime();
BENCHMARK_CAPTURE(SuiteExecution, Overlap, kOverlap, 1)->Name("testing/overlap_3x3")->UseRealTime();
BENCHMARK(CacheBatch)->Name("testing/cache_batch")->Arg(1)->Arg(16)->UseRealTime();
BENCHMARK(ConcurrentSuites)->Name("testing/concurrent_suites")->Arg(1)->Arg(4)->UseRealTime();

}  // namespace
}  // namespace vela::benchmarks
