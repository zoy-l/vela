#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"

#include "benchmarks/native_workloads.h"
#include "compiler/compiler.h"
#include "compiler/frontend/source_loader.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::benchmarks {
namespace {

struct Problem final {
  std::int64_t size;
  std::int64_t expected;
};

struct Workload final {
  std::string_view name;
  NativeWorkload native;
  std::array<Problem, 2> problems;
};

constexpr auto kWorkloads = std::to_array<Workload>({
    {
        .name = "sieve",
        .native = Sieve,
        .problems = {{{.size = 10000, .expected = 1229}, {.size = 100000, .expected = 9592}}},
    },
    {
        .name = "mandelbrot",
        .native = Mandelbrot,
        .problems = {{{.size = 64, .expected = 99673}, {.size = 128, .expected = 399752}}},
    },
    {
        .name = "quicksort",
        .native = Quicksort,
        .problems =
            {
                {
                    {.size = 1024, .expected = 21409011239},
                    {.size = 8192, .expected = 1455713821118},
                },
            },
    },
    {
        .name = "binary_trees",
        .native = BinaryTrees,
        .problems = {{{.size = 6, .expected = 2159}, {.size = 9, .expected = 17391}}},
    },
    {
        .name = "particles",
        .native = Particles,
        .problems = {{{.size = 128, .expected = 8411000}, {.size = 1024, .expected = 74921000}}},
    },
    {
        .name = "word_count",
        .native = WordCount,
        .problems = {{{.size = 256, .expected = 1088}, {.size = 2048, .expected = 65600}}},
    },
    {
        .name = "closure_pipeline",
        .native = ClosurePipeline,
        .problems = {{{.size = 1024, .expected = 28986399}, {.size = 8192, .expected = 264132249}}},
    },
    {
        .name = "array_growth",
        .native = ArrayGrowth,
        .problems =
            {
                {
                    {.size = 1024, .expected = 1047552},
                    {.size = 65536, .expected = 4294901760},
                },
            },
    },
    {
        .name = "array_growth_reserved",
        .native = ArrayGrowthReserved,
        .problems =
            {
                {
                    {.size = 1024, .expected = 1047552},
                    {.size = 65536, .expected = 4294901760},
                },
            },
    },
});

struct Sources final {
  std::string path;
  std::string workload;
  std::string input;

  compiler::CompileResult Compile(runtime::Isolate& isolate) const {
    const auto modules = std::to_array<frontend::SourceInput>({
        {.path = path, .text = workload},
        {.path = "workloads/input.vela", .text = input},
    });
    return compiler::CompileModules(modules, path, isolate);
  }
};

std::optional<std::string> ReadSource(benchmark::State& state, std::string_view filename) {
  const auto path = std::filesystem::path(VELA_BENCHMARK_SOURCE_DIR) / filename;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    state.SkipWithError("Cannot open benchmark source: " + path.string());
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::optional<Sources> LoadSources(benchmark::State& state,
                                   const Workload& workload,
                                   Problem problem) {
  const auto path = "workloads/" + std::string(workload.name) + ".vela";
  auto source = ReadSource(state, path);
  auto input = ReadSource(state, "workloads/input.vela");
  if (!source || !input) {
    return std::nullopt;
  }
  // 包装入口只提供负载规模；保留独立脚本中的 main，便于直接运行相同实现。
  *source += "\nfunc benchmarkSetup(): void { data.prepare(" + std::to_string(problem.size) +
             "); }\nfunc benchmarkEntry(): int { return run(); }\n";
  return Sources{.path = path, .workload = std::move(*source), .input = std::move(*input)};
}

void ReportDiagnostics(benchmark::State& state, const compiler::CompileResult& compiled) {
  std::string message;
  for (const auto& diagnostic : compiled.diagnostics) {
    message += diagnostic.source_path + ":" + std::to_string(diagnostic.location.line) + ":" +
               std::to_string(diagnostic.location.column) + ": " + diagnostic.message + "\n";
  }
  state.SkipWithError(message);
}

bool IsExpected(const vm::ExecutionResult& result, Problem problem) {
  return result.Succeeded() && result.value.IsInteger() &&
         result.value.AsInteger() == problem.expected;
}

void Compile(benchmark::State& state, Workload workload, Problem problem) {
  const auto sources = LoadSources(state, workload, problem);
  if (!sources) {
    return;
  }
  for (auto iteration : state) {
    runtime::Isolate isolate;
    auto compiled = sources->Compile(isolate);
    if (!compiled.Succeeded()) {
      ReportDiagnostics(state, compiled);
      return;
    }
    benchmark::DoNotOptimize(compiled);
  }
  state.counters["source_bytes"] =
      static_cast<double>(sources->workload.size() + sources->input.size());
}

void Vm(benchmark::State& state, Workload workload, Problem problem) {
  const auto sources = LoadSources(state, workload, problem);
  if (!sources) {
    return;
  }
  runtime::Isolate isolate;
  const auto compiled = sources->Compile(isolate);
  if (!compiled.Succeeded()) {
    ReportDiagnostics(state, compiled);
    return;
  }
  vm::VirtualMachine machine(isolate);
  const auto setup = machine.Execute(compiled.program.FindFunction("benchmarkSetup"));
  if (!setup.Succeeded()) {
    state.SkipWithError("Input preparation failed: " + setup.message);
    return;
  }
  const auto entry = compiled.program.FindFunction("benchmarkEntry");
  const auto warm = machine.Execute(entry);
  if (!IsExpected(warm, problem)) {
    state.SkipWithError("Warm-up failed or checksum mismatched: " + warm.message);
    return;
  }
  auto& heap = isolate.heap();
  // 先结束初始化和 warm-up 的垃圾周期，之后让生产 GC 自行触发，不按样本强制清理。
  heap.FinishCollection();
  heap.Collect();
  const auto baseline_bytes = heap.allocated_bytes();
  state.counters["baseline_bytes"] = static_cast<double>(baseline_bytes);
  state.counters["register_bytes"] = static_cast<double>(machine.register_bytes());
  for (auto iteration : state) {
    auto result = machine.Execute(entry);
    benchmark::DoNotOptimize(result);
    if (!IsExpected(result, problem)) {
      state.SkipWithError("Execution failed or checksum mismatched: " + result.message);
      return;
    }
  }
  state.counters["allocated_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["committed_bytes"] = static_cast<double>(heap.committed_bytes());
  heap.FinishCollection();
  heap.Collect();
  state.counters["retained_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters["result"] = static_cast<double>(problem.expected);
}

void Cpp(benchmark::State& state, Workload workload, Problem problem) {
  std::vector<std::int64_t> input(static_cast<std::size_t>(problem.size));
  std::ranges::iota(input, std::int64_t{0});
  if (workload.native(input) != problem.expected) {
    state.SkipWithError("C++ warm-up checksum mismatched");
    return;
  }
  for (auto iteration : state) {
    // 原生内核独立编译；输入屏障和结果屏障阻止编译器跨迭代合并整个负载。
    benchmark::DoNotOptimize(input.data());
    auto result = workload.native(input);
    benchmark::DoNotOptimize(result);
    if (result != problem.expected) {
      state.SkipWithError("C++ checksum mismatched");
      return;
    }
  }
  state.counters["result"] = static_cast<double>(problem.expected);
}

int RegisterWorkloads() {
  for (const auto& workload : kWorkloads) {
    for (const auto problem : workload.problems) {
      const auto suffix = std::string(workload.name) + "/" + std::to_string(problem.size);
      benchmark::RegisterBenchmark("compare/cpp/" + suffix, Cpp, workload, problem)->UseRealTime();
      benchmark::RegisterBenchmark("compare/vm/" + suffix, Vm, workload, problem)->UseRealTime();
      benchmark::RegisterBenchmark("compile/representative/" + suffix, Compile, workload, problem)
          ->UseRealTime();
    }
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = RegisterWorkloads();

}  // namespace
}  // namespace vela::benchmarks
