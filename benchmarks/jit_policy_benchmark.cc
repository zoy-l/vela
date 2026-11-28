#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <benchmark/benchmark.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

#include "base/execution_status.h"
#include "base/file.h"
#include "base/native_program_mode.h"
#include "base/path.h"
#include "benchmarks/temporary_directory.h"
#include "compiler/development/development_files_local.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/ir/preparation.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/compiler.h"
#include "compiler/native/call_targets.h"
#include "compiler/native/code_generator.h"
#include "compiler/native/compile_options.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/field_access.h"
#include "compiler/native/native_roots.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/value.h"

namespace vela::benchmarks {
namespace {

using compiler::NativeOptimization;
namespace native = compiler::native;

constexpr std::size_t kPages = 128;
constexpr std::int64_t kPageLoopSum = 43;

enum class MainResult : std::uint8_t { Integer, Printed };

struct Workload final {
  std::string_view name;
  std::string_view filename;
  std::string_view entry;
  std::int64_t expected;
  MainResult main_result;
};

constexpr auto kWorkloads = std::to_array<Workload>({
    {
        .name = "library",
        .filename = "library_linking.vela",
        .entry = "main",
        .expected = 6,
        .main_result = MainResult::Integer,
    },
    {
        .name = "sieve",
        .filename = "workloads/sieve.vela",
        .entry = "run",
        .expected = 1229,
        .main_result = MainResult::Printed,
    },
    {
        .name = "particles",
        .filename = "workloads/particles.vela",
        .entry = "run",
        .expected = 8411000,
        .main_result = MainResult::Printed,
    },
    {
        .name = "pages128",
        .filename = "",
        .entry = "main",
        .expected = static_cast<std::int64_t>(kPages - 1) + (2 * kPageLoopSum),
        .main_result = MainResult::Integer,
    },
});

struct Policy final {
  std::string_view name;
  NativeOptimization optimization;
};

constexpr auto kPolicies = std::to_array<Policy>({
    {.name = "O3", .optimization = NativeOptimization::Release},
    {.name = "baseline", .optimization = NativeOptimization::Baseline},
    {.name = "O0", .optimization = NativeOptimization::None},
});

// 输出流的堆地址不随结果移动而变化，并覆盖借用它的整个会话。
struct PreparedSession final {
  std::unique_ptr<std::ostringstream> output;
  std::unique_ptr<compiler::DevelopmentSession> session;
};

std::expected<std::filesystem::path, std::string> PreparePath(
    const Workload& workload,
    const std::filesystem::path& directory) {
  if (!workload.filename.empty()) {
    return std::filesystem::path(VELA_BENCHMARK_SOURCE_DIR) / workload.filename;
  }
  std::string main;
  for (std::size_t index = 0; index < kPages; ++index) {
    const auto name = "page" + std::to_string(index);
    main += "import '" + name + "';\n";
    const auto text = "export func " + name +
                      "(seed: int): int {\n"
                      "let value = seed;\n"
                      "for (let index = 0; index < 16; index = index + 1) { value += index % 7; }\n"
                      "return value; }\n";
    const auto written =
        WriteFileAtomically(directory / (name + ".vela"), std::as_bytes(std::span(text)));
    if (!written) {
      return std::unexpected(written.error());
    }
  }
  const auto last = std::to_string(kPages - 1);
  main += "func main(): int { return page0(0) + page" + last + "(" + last + "); }\n";
  auto path = directory / "main.vela";
  const auto written = WriteFileAtomically(path, std::as_bytes(std::span(main)));
  if (!written) {
    return std::unexpected(written.error());
  }
  return path;
}

std::expected<PreparedSession, std::string> PrepareSession(const std::filesystem::path& path,
                                                           NativeOptimization optimization) {
  const auto entry = PathText(path);
  compiler::LocalDevelopmentFiles files(entry);
  auto update = files.Capture(compiler::CompiledSourceSnapshot{});
  if (!update) {
    return std::unexpected(frontend::DiagnosticsText(update.error()));
  }
  compiler::DevelopmentOptions options;
  options.compilation.optimization = optimization;
  options.compilation.debugging = native::NativeDebugging::SourceLines;
  options.compilation.tier = NativeCodeTier::Baseline;
  options.compilation.materialization = compiler::jit::Materialization::Eager;
  auto created = compiler::DevelopmentSession::Create(files.entry(), {}, std::move(options));
  if (!created) {
    return std::unexpected(frontend::DiagnosticsText(created.error()));
  }
  auto& session = **created;
  auto candidate = session.Prepare(std::move(*update), compiler::DevelopmentOperation::Load);
  if (!candidate) {
    return std::unexpected(candidate.error().message +
                           frontend::DiagnosticsText(candidate.error().diagnostics));
  }
  (void)session.Submit(std::move(*candidate));
  const auto result = session.Pump();
  if (result.status != compiler::DevelopmentStatus::Accepted) {
    return std::unexpected(result.message + frontend::DiagnosticsText(result.diagnostics));
  }
  PreparedSession prepared{
      .output = std::make_unique<std::ostringstream>(),
      .session = std::move(*created),
  };
  prepared.session->isolate().SetOutput(*prepared.output);
  return prepared;
}

std::expected<void, std::string> VerifyStartup(PreparedSession& prepared,
                                               const Workload& workload) {
  const auto value = prepared.session->RunEntry();
  if (!value) {
    return std::unexpected(std::string(ExecutionStatusName(value.error())));
  }
  if (workload.main_result == MainResult::Printed) {
    if (prepared.output->str() != std::to_string(workload.expected) + "\n") {
      return std::unexpected("Startup output checksum mismatch");
    }
  } else if (!value->IsInteger() || value->AsInteger() != workload.expected) {
    return std::unexpected("Startup result checksum mismatch");
  }
  return {};
}

std::expected<bool, std::string> ProbeFastISel(NativeOptimization optimization) {
  const std::array input{
      frontend::SourceInput{.path = "main.vela", .text = "func main(): int { return 42; }"},
  };
  auto prepared = ir::PrepareParsed(frontend::LoadSources(input, "main.vela"));
  if (!prepared.Succeeded()) {
    return std::unexpected(frontend::DiagnosticsText(prepared.diagnostics));
  }
  const auto calls = native::PlanNativeCalls(prepared.program, NativeCodeTier::Baseline);
  const auto fields = native::FindFieldTargets(prepared.program);
  auto module = native::Generate(prepared.program, calls, fields, "policy_probe",
                                 native::NativeDebugging::Safepoints, NativeCodeTier::Baseline,
                                 native::EscapeOptimization::Enabled,
                                 compiler::NativeCodePlacement::Dynamic, optimization);
  if (!module) {
    return std::unexpected(frontend::DiagnosticsText(module.error()));
  }
  if (const auto finalized = native::Finalize(*module); !finalized) {
    return std::unexpected(finalized.error());
  }
  const auto object = native::EmitObject(*module);
  if (!object) {
    return std::unexpected(object.error());
  }
  if ((*object)->getBufferSize() == 0) {
    return std::unexpected("Instruction-selector probe emitted an empty object");
  }
  return module->machine->Options.EnableFastISel != 0;
}

void Startup(benchmark::State& state, Workload workload, Policy policy) {
  const BenchmarkDirectory directory(state);
  if (!directory.IsValid()) {
    return;
  }
  const auto path = PreparePath(workload, directory.path());
  const auto fast_isel = ProbeFastISel(policy.optimization);
  if (!path || !fast_isel) {
    state.SkipWithError(!path ? path.error() : fast_isel.error());
    return;
  }
  compiler::DevelopmentTimings totals;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    {
      auto prepared = PrepareSession(*path, policy.optimization);
      state.PauseTiming();
      if (!prepared) {
        state.ResumeTiming();
        state.SkipWithError(prepared.error());
        return;
      }
      const auto checked = VerifyStartup(*prepared, workload);
      if (!checked) {
        state.ResumeTiming();
        state.SkipWithError(checked.error());
        return;
      }
      const auto& timings = prepared->session->result().timings;
      totals.source += timings.source;
      totals.native.generation += timings.native.generation;
      totals.native.optimization += timings.native.optimization;
      totals.native.compilation += timings.native.compilation;
      totals.native.linking += timings.native.linking;
      state.counters["functions"] =
          static_cast<double>(prepared->session->code_resources().functions);
    }
    state.ResumeTiming();
  }
  const auto average = benchmark::Counter::kAvgIterations;
  state.counters["source_ns"] =
      benchmark::Counter(static_cast<double>(totals.source.count()), average);
  state.counters["llvm_generation_ns"] =
      benchmark::Counter(static_cast<double>(totals.native.generation.count()), average);
  state.counters["llvm_optimization_ns"] =
      benchmark::Counter(static_cast<double>(totals.native.optimization.count()), average);
  state.counters["machine_compilation_ns"] =
      benchmark::Counter(static_cast<double>(totals.native.compilation.count()), average);
  state.counters["linking_ns"] =
      benchmark::Counter(static_cast<double>(totals.native.linking.count()), average);
  state.counters["fast_isel_enabled"] = *fast_isel ? 1.0 : 0.0;
}

void Steady(benchmark::State& state, Workload workload, Policy policy) {
  const BenchmarkDirectory directory(state);
  if (!directory.IsValid()) {
    return;
  }
  const auto path = PreparePath(workload, directory.path());
  if (!path) {
    state.SkipWithError(path.error());
    return;
  }
  auto prepared = PrepareSession(*path, policy.optimization);
  if (!prepared) {
    state.SkipWithError(prepared.error());
    return;
  }
  const auto checked = VerifyStartup(*prepared, workload);
  if (!checked) {
    state.SkipWithError(checked.error());
    return;
  }
  auto& session = *prepared->session;
  session.isolate().heap().FinishCollection();
  session.isolate().heap().Collect();
  const compiler::DevelopmentEntry entry{.module = {}, .name = workload.entry};
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    auto value = session.InvokeEntry(entry);
    benchmark::DoNotOptimize(value);
    if (!value || !value->IsInteger() || value->AsInteger() != workload.expected) {
      state.SkipWithError("Steady execution checksum mismatch");
      return;
    }
  }
}

int RegisterPolicies() {
  for (const auto& workload : kWorkloads) {
    for (const auto& policy : kPolicies) {
      const auto suffix = std::string(workload.name) + "/" + std::string(policy.name);
      benchmark::RegisterBenchmark("jit_policy/startup/" + suffix, Startup, workload, policy)
          ->UseRealTime();
      benchmark::RegisterBenchmark("jit_policy/steady/" + suffix, Steady, workload, policy)
          ->UseRealTime();
    }
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = RegisterPolicies();

}  // namespace
}  // namespace vela::benchmarks
