#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"

#include "benchmarks/heap_metrics.h"
#include "benchmarks/native_standard_workloads.h"
#include "benchmarks/native_workloads.h"
#include "benchmarks/source.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_execution.h"

namespace vela::benchmarks {
namespace {

constexpr std::string_view kInputModule = "workloads/input.vela";
constexpr std::string_view kSetupEntry = "benchmarkSetup";
constexpr std::string_view kWorkloadEntry = "benchmarkEntry";

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
    {
        .name = "dynamic_values",
        .native = DynamicValues,
        .problems = {{{.size = 256, .expected = 11390}, {.size = 2048, .expected = 702123}}},
    },
    {
        .name = "dynamic_dispatch",
        .native = DynamicDispatch,
        .problems = {{{.size = 256, .expected = 2796160}, {.size = 2048, .expected = 1431655424}}},
    },
    {
        .name = "json_roundtrip",
        .native = JsonRoundtrip,
        .problems = {{{.size = 64, .expected = 5911}, {.size = 1024, .expected = 587179}}},
    },
    {
        .name = "byte_data_little",
        .native = ByteDataLittle,
        .problems = {{{.size = 128, .expected = 8255}, {.size = 2048, .expected = 2096383}}},
    },
    {
        .name = "byte_data_big",
        .native = ByteDataBig,
        .problems = {{{.size = 128, .expected = 8128}, {.size = 2048, .expected = 2096128}}},
    },
    {
        .name = "regexp_records",
        .native = RegExpRecords,
        .problems = {{{.size = 32, .expected = 6304}, {.size = 1024, .expected = 6296576}}},
    },
    {
        .name = "numeric_parse",
        .native = NumericParse,
        .problems = {{{.size = 256, .expected = 65280}, {.size = 2048, .expected = 4192256}}},
    },
    {
        .name = "ascii_case",
        .native = AsciiCase,
        .problems = {{{.size = 64, .expected = 438}, {.size = 1024, .expected = 8106}}},
    },
    {
        .name = "utc_roundtrip",
        .native = UtcRoundtrip,
        .problems = {{{.size = 64, .expected = 2023344}, {.size = 1024, .expected = 524854188}}},
    },
});

struct Sources final {
  std::string path;
  std::string workload;
  std::string input;

  compiler::jit::SourceResult Compile(compiler::jit::Compiler& compiler) const {
    const auto modules = std::to_array<frontend::SourceInput>({
        {.path = path, .text = workload},
        {.path = kInputModule, .text = input},
    });
    return compiler::jit::CompileModules(compiler, modules, path);
  }
};

std::optional<Sources> LoadSources(benchmark::State& state,
                                   const Workload& workload,
                                   Problem problem) {
  const auto path = "workloads/" + std::string(workload.name) + ".vela";
  auto source = LoadBenchmarkSource(state, path);
  auto input = LoadBenchmarkSource(state, kInputModule);
  if (!source || !input) {
    return std::nullopt;
  }
  // 包装入口只提供负载规模；保留独立脚本中的 main，便于直接运行相同实现。
  source->append("\nfunc ")
      .append(kSetupEntry)
      .append("(): void { data.prepare(")
      .append(std::to_string(problem.size))
      .append("); }\nfunc ")
      .append(kWorkloadEntry)
      .append("(): int { return run(); }\n");
  return Sources{.path = path, .workload = std::move(*source), .input = std::move(*input)};
}

bool IsExpected(const runtime::ExecutionResult& result, Problem problem) {
  return result.Succeeded() && result.value.IsInteger() &&
         result.value.AsInteger() == problem.expected;
}

void Compile(benchmark::State& state, Workload workload, Problem problem) {
  const auto sources = LoadSources(state, workload, problem);
  if (!sources) {
    return;
  }
  auto compiler = compiler::jit::Compiler::Create();
  if (!compiler) {
    state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
    return;
  }
  for (auto iteration : state) {
    auto compiled = sources->Compile(**compiler);
    if (!compiled.Succeeded()) {
      state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
      return;
    }
    benchmark::DoNotOptimize(compiled);
  }
  state.counters["source_bytes"] =
      static_cast<double>(sources->workload.size() + sources->input.size());
}

void Jit(benchmark::State& state, Workload workload, Problem problem) {
  const auto sources = LoadSources(state, workload, problem);
  if (!sources) {
    return;
  }
  runtime::Isolate isolate;
  auto compiler = compiler::jit::Compiler::Create();
  if (!compiler) {
    state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
    return;
  }
  const auto compiled = sources->Compile(**compiler);
  if (!compiled.Succeeded()) {
    state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
    return;
  }
  const auto loaded = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  if (!loaded) {
    state.SkipWithError(loaded.error().message);
    return;
  }
  auto& instance = **loaded;
  const auto setup_entry = instance.FindFunction(kSetupEntry);
  const auto workload_entry = instance.FindFunction(kWorkloadEntry);
  if (!setup_entry || !workload_entry) {
    state.SkipWithError("Missing workload entry");
    return;
  }
  const auto setup = runtime::ExecuteNativeFunction(instance, *setup_entry);
  if (!setup.Succeeded()) {
    state.SkipWithError("Input preparation failed: " + setup.message);
    return;
  }
  const auto entry = *workload_entry;
  const auto warm = runtime::ExecuteNativeFunction(instance, entry);
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
  for (auto iteration : state) {
    auto result = runtime::ExecuteNativeFunction(instance, entry);
    benchmark::DoNotOptimize(result);
    if (!IsExpected(result, problem)) {
      state.SkipWithError("Execution failed or checksum mismatched: " + result.message);
      return;
    }
  }
  const auto retained = ReportHeap(state, heap);
  if (retained != baseline_bytes) {
    state.SkipWithError("Execution retained temporary heap values after collection");
    return;
  }
  state.counters["result"] = static_cast<double>(problem.expected);
}

void FirstJit(benchmark::State& state, Workload workload, Problem problem) {
  const auto sources = LoadSources(state, workload, problem);
  if (!sources) {
    return;
  }
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    state.PauseTiming();
    {
      runtime::Isolate isolate;
      auto compiler = compiler::jit::Compiler::Create();
      if (!compiler) {
        state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
        return;
      }
      const auto compiled = sources->Compile(**compiler);
      if (!compiled.Succeeded()) {
        state.ResumeTiming();
        state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
        return;
      }
      const auto loaded = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
      if (!loaded) {
        state.SkipWithError(loaded.error().message);
        return;
      }
      auto& instance = **loaded;
      const auto setup_entry = instance.FindFunction(kSetupEntry);
      const auto workload_entry = instance.FindFunction(kWorkloadEntry);
      if (!setup_entry || !workload_entry) {
        state.SkipWithError("Missing workload entry");
        return;
      }
      const auto setup = runtime::ExecuteNativeFunction(instance, *setup_entry);
      if (!setup.Succeeded()) {
        state.ResumeTiming();
        state.SkipWithError("Input preparation failed: " + setup.message);
        return;
      }
      const auto entry = *workload_entry;
      state.ResumeTiming();
      auto result = runtime::ExecuteNativeFunction(instance, entry);
      benchmark::DoNotOptimize(result);
      state.PauseTiming();
      if (!IsExpected(result, problem)) {
        state.ResumeTiming();
        state.SkipWithError("First execution failed or checksum mismatched: " + result.message);
        return;
      }
    }
    state.ResumeTiming();
  }
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
      benchmark::RegisterBenchmark("compare/jit/" + suffix, Jit, workload, problem)->UseRealTime();
      benchmark::RegisterBenchmark("compile/representative/" + suffix, Compile, workload, problem)
          ->UseRealTime();
      benchmark::RegisterBenchmark("first/jit/" + suffix, FirstJit, workload, problem)
          ->UseRealTime();
    }
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = RegisterWorkloads();

}  // namespace
}  // namespace vela::benchmarks
