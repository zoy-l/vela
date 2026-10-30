#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"

#include "base/file.h"
#include "base/path.h"
#include "compiler/compilation_cache.h"
#include "compiler/compiler.h"
#include "compiler/testing/result.h"
#include "compiler/testing/suite.h"
#include "compiler/tooling/mock_generator.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::benchmarks {
namespace {

enum class Generation : std::uint8_t { Write, Check };

class MockProject final {
 public:
  MockProject() = default;
  ~MockProject() {
    if (!directory_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(directory_, error);
    }
  }
  MockProject(const MockProject&) = delete;
  MockProject& operator=(const MockProject&) = delete;
  MockProject(MockProject&&) = delete;
  MockProject& operator=(MockProject&&) = delete;

  std::expected<void, std::string> Prepare();
  std::expected<void, std::string> Generate(Generation mode) const;
  [[nodiscard]] std::string source() const { return PathText(directory_ / "mock_workload.vela"); }
  [[nodiscard]] std::filesystem::path cache() const { return directory_ / "cache"; }

 private:
  std::filesystem::path directory_;
};

std::expected<void, std::string> MockProject::Prepare() {
  std::error_code error;
  const auto temporary = std::filesystem::temp_directory_path(error);
  if (error) {
    return std::unexpected(error.message());
  }
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = temporary / ("vela-mock-benchmark-" + std::to_string(suffix));
  if (!std::filesystem::create_directory(directory, error)) {
    return std::unexpected(error ? error.message() : "mock benchmark directory already exists");
  }
  directory_ = directory;
  for (const auto* file : {"mock_api.vela", "mock_workload.vela"}) {
    const auto contents = ReadBinaryFile(std::filesystem::path(VELA_BENCHMARK_SOURCE_DIR) / file);
    if (!contents) {
      return std::unexpected(contents.error());
    }
    const auto written = WriteFileAtomically(directory_ / file, *contents);
    if (!written) {
      return written;
    }
  }
  return Generate(Generation::Write);
}

std::expected<void, std::string> MockProject::Generate(Generation mode) const {
  std::vector<std::string> arguments{
      PathText(directory_ / "mock_api.vela"),       "--interface", "MockBenchmarkService", "-o",
      PathText(directory_ / "mock_api.mocks.vela"),
  };
  if (mode == Generation::Check) {
    arguments.emplace_back("--check");
  }
  std::vector<char*> values;
  values.reserve(arguments.size());
  for (auto& argument : arguments) {
    values.push_back(argument.data());
  }
  std::ostringstream output;
  std::ostringstream errors;
  if (tooling::MockCommand(values, output, errors) != 0) {
    return std::unexpected(errors.str());
  }
  return {};
}

std::expected<void, std::string> Execute(runtime::Isolate& isolate,
                                         const vm::Program& program,
                                         std::string_view name) {
  testing::SuiteOptions options;
  options.select = [name](const testing::TestResult& test) -> std::expected<bool, std::string> {
    return test.name == name;
  };
  std::ostringstream diagnostics;
  testing::Suite suite(isolate, program, std::move(options), diagnostics);
  const auto result = suite.Run();
  if (result.exit_code != 0 || !result.errors.empty() || result.tests.size() != 1) {
    return std::unexpected("mock benchmark failed to load or select its test");
  }
  const auto& test = result.tests.front();
  if (test.status != testing::TestStatus::Passed) {
    std::string message = test.name;
    for (const auto& failure : test.errors) {
      message += "\n" + failure.message;
    }
    return std::unexpected(message);
  }
  return {};
}

void GenerationTime(benchmark::State& state, Generation mode) {
  MockProject project;
  if (const auto prepared = project.Prepare(); !prepared) {
    state.SkipWithError(prepared.error());
    return;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    if (const auto generated = project.Generate(mode); !generated) {
      state.SkipWithError(generated.error());
      return;
    }
  }
}

enum class Compilation : std::uint8_t { Source, Cache };

void CompilationTime(benchmark::State& state, Compilation mode) {
  MockProject project;
  if (const auto prepared = project.Prepare(); !prepared) {
    state.SkipWithError(prepared.error());
    return;
  }
  const auto path = project.source();
  const auto cache = project.cache();
  {
    runtime::Isolate isolate;
    const auto populated = compiler::CompileFileCached(path, cache, isolate);
    if (!populated || !populated->compiled.Succeeded() ||
        !Execute(isolate, populated->compiled.program, "configured")) {
      state.SkipWithError("cannot prepare generated mock compilation benchmark");
      return;
    }
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    runtime::Isolate isolate;
    if (mode == Compilation::Cache) {
      auto compiled = compiler::CompileFileCached(path, cache, isolate);
      if (!compiled || compiled->status != compiler::CacheStatus::Hit ||
          !compiled->compiled.Succeeded()) {
        state.SkipWithError("generated mock cache did not hit");
        return;
      }
      benchmark::DoNotOptimize(compiled);
    } else {
      auto compiled = compiler::CompileFile(path, isolate);
      if (!compiled.Succeeded()) {
        state.SkipWithError("generated mock compilation failed");
        return;
      }
      benchmark::DoNotOptimize(compiled);
    }
  }
}

void ExecutionTime(benchmark::State& state, std::string_view name, std::int64_t operations) {
  MockProject project;
  if (const auto prepared = project.Prepare(); !prepared) {
    state.SkipWithError(prepared.error());
    return;
  }
  runtime::Isolate isolate;
  const auto compiled = compiler::CompileFile(project.source(), isolate);
  if (!compiled.Succeeded()) {
    state.SkipWithError("cannot compile mock workload");
    return;
  }
  const auto& program = compiled.program;
  if (const auto warmup = Execute(isolate, program, name); !warmup) {
    state.SkipWithError(warmup.error());
    return;
  }
  auto& heap = isolate.heap();
  heap.FinishCollection();
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    if (const auto executed = Execute(isolate, program, name); !executed) {
      state.SkipWithError(executed.error());
      return;
    }
  }
  state.counters["allocated_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["committed_bytes"] = static_cast<double>(heap.committed_bytes());
  const auto comparisons =
      vm::VirtualMachine(isolate).Execute(program.FindFunction("comparisonCount"));
  if (!comparisons.Succeeded() || !comparisons.value.IsInteger()) {
    state.SkipWithError("missing matcher comparison evidence");
    return;
  }
  state.counters["custom_matcher_evaluations"] = static_cast<double>(comparisons.value.AsInteger());
  heap.FinishCollection();
  heap.Collect();
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["baseline_bytes"] = static_cast<double>(baseline);
  state.counters["remaining_frames"] =
      static_cast<double>(isolate.scheduler().suspended_frame_count());
  if (heap.allocated_bytes() != baseline || isolate.scheduler().suspended_frame_count() != 0) {
    state.SkipWithError("completed mock suite retained history, callbacks or suspended frames");
    return;
  }
  state.SetItemsProcessed(state.iterations() * operations);
}

BENCHMARK_CAPTURE(GenerationTime, Write, Generation::Write)->Name("mock/generate")->UseRealTime();
BENCHMARK_CAPTURE(GenerationTime, Check, Generation::Check)->Name("mock/check")->UseRealTime();
BENCHMARK_CAPTURE(CompilationTime, Source, Compilation::Source)
    ->Name("mock/compile")
    ->UseRealTime();
BENCHMARK_CAPTURE(CompilationTime, Cache, Compilation::Cache)
    ->Name("mock/cache_hit")
    ->UseRealTime();
BENCHMARK_CAPTURE(ExecutionTime, Manual, "manual", 1024)->Name("mock/manual_1024")->UseRealTime();
BENCHMARK_CAPTURE(ExecutionTime, ManualFuture, "manual_future", 1024)
    ->Name("mock/manual_future_1024")
    ->UseRealTime();
BENCHMARK_CAPTURE(ExecutionTime, Configured, "configured", 1024)
    ->Name("mock/configured_1024")
    ->UseRealTime();
BENCHMARK_CAPTURE(ExecutionTime, Rules1, "rules_1", 1024)->Name("mock/rules_1_1024")->UseRealTime();
BENCHMARK_CAPTURE(ExecutionTime, Rules16, "rules_16", 1024)
    ->Name("mock/rules_16_1024")
    ->UseRealTime();
BENCHMARK_CAPTURE(ExecutionTime, Rules128, "rules_128", 1024)
    ->Name("mock/rules_128_1024")
    ->UseRealTime();
BENCHMARK_CAPTURE(ExecutionTime, Verify, "verify", 1024)->Name("mock/verify_1024")->UseRealTime();
BENCHMARK_CAPTURE(ExecutionTime, Order, "order", 1024)->Name("mock/order_1024")->UseRealTime();
BENCHMARK_CAPTURE(ExecutionTime, Capture, "capture", 1024)
    ->Name("mock/capture_1024")
    ->UseRealTime();
BENCHMARK_CAPTURE(ExecutionTime, Future, "future", 1024)->Name("mock/future_1024")->UseRealTime();
BENCHMARK_CAPTURE(ExecutionTime, Wait, "wait", 128)->Name("mock/wait_128")->UseRealTime();
BENCHMARK_CAPTURE(ExecutionTime, Resource, "resource", 10000)
    ->Name("mock/resource_10000")
    ->UseRealTime();

}  // namespace
}  // namespace vela::benchmarks
