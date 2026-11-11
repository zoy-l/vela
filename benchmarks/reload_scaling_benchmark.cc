#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <benchmark/benchmark.h>

#include "base/native_program_mode.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "benchmarks/jit_benchmark.h"

namespace vela::benchmarks {
namespace {

using Clock = std::chrono::steady_clock;
using compiler::DevelopmentOperation;
using compiler::DevelopmentStatus;

enum class ModuleGraph : std::uint8_t { Independent, Imports, Fanout };
constexpr std::int64_t kProbeAnswer = 42;
constexpr std::int64_t kMediumProjectFiles = 32;
constexpr std::int64_t kLargeProjectFiles = 256;
constexpr std::int64_t kMaximumProjectFiles = 2048;

std::string ValueSource(std::int64_t version) {
  return "export func value(): int { return " + std::to_string(version) +
         "; }\nfunc probe(): int { return 42; }";
}

compiler::SourceUpdate InitialSources(std::int64_t files, ModuleGraph graph) {
  compiler::SourceUpdate initial;
  const auto independent = graph == ModuleGraph::Independent;
  const auto* const imported =
      "import 'dependency.vela' as dependency; "
      "export func value(): int { return dependency.value(); }\n"
      "func probe(): int { return 42; }";
  initial.changes.push_back(
      {.path = "entry.vela", .text = independent ? ValueSource(0) : imported});
  for (std::int64_t index = 1; index < files; ++index) {
    if (!independent && index == 1) {
      initial.changes.push_back({.path = "dependency.vela", .text = ValueSource(0)});
      continue;
    }
    auto path = "module_" + std::to_string(index) + ".vela";
    initial.added_roots.push_back(path);
    initial.changes.push_back({
        .path = std::move(path),
        .text = graph == ModuleGraph::Fanout
                    ? imported
                    : "export func value(input: int): int { return input + " +
                          std::to_string(index) +
                          "; } func twice(input: int): int { return input * 2; }\n"
                          "func probe(): int { return 42; }",
    });
  }
  return initial;
}

void ReloadModules(benchmark::State& state, NativeCodeTier tier, ModuleGraph graph) {
  compiler::DevelopmentOptions options;
  options.compilation.tier = tier;
  auto created = compiler::DevelopmentSession::Create("entry.vela", {}, std::move(options));
  if (!created) {
    state.SkipWithError(frontend::DiagnosticsText(created.error()));
    return;
  }
  auto& session = **created;
  const auto files = state.range(0);
  const auto independent = graph == ModuleGraph::Independent;
  auto initial = InitialSources(files, graph);
  const auto probe_module = initial.changes.back().path;
  constexpr std::int64_t kExpectedCompiled = 1;
  auto loaded = session.Prepare(std::move(initial), DevelopmentOperation::Load);
  if (!loaded) {
    state.SkipWithError(frontend::DiagnosticsText(loaded.error().diagnostics));
    return;
  }
  static_cast<void>(session.Submit(std::move(*loaded)));
  if (session.Pump().status != DevelopmentStatus::Accepted) {
    state.SkipWithError("cannot load scaling project");
    return;
  }
  std::int64_t version = 0;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    ++version;
    const auto started = Clock::now();
    auto updated = PublishDevelopment(session, independent ? "entry.vela" : "dependency.vela",
                                      ValueSource(version), DevelopmentOperation::HotReload);
    if (!updated) {
      state.SkipWithError(updated.error());
      return;
    }
    const auto& result = *updated;
    const auto elapsed = Clock::now() - started;
    state.SetIterationTime(std::chrono::duration<double>(elapsed).count());
    const auto current = session.Invoke("entry.vela", "value");
    if (!current || current->AsInteger() != version) {
      state.SkipWithError("incremental entry checksum mismatch");
      return;
    }
    if (std::cmp_not_equal(result.changes.compiled_functions, kExpectedCompiled)) {
      state.SkipWithError("incremental compilation invalidated an unchanged caller");
      return;
    }
    const auto entering = Clock::now();
    const auto probe = session.InvokeEntry({.module = probe_module, .name = "probe"});
    const auto entry_time = Clock::now() - entering;
    if (!probe || probe->AsInteger() != kProbeAnswer) {
      state.SkipWithError("incremental entry lookup checksum mismatch");
      return;
    }
    state.counters["entry_ns"] += static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(entry_time).count());
    const auto& timings = result.timings;
    state.counters["source_ns"] += static_cast<double>(timings.source.count());
    state.counters["source_loading_ns"] += static_cast<double>(timings.frontend.loading.count());
    state.counters["type_checking_ns"] += static_cast<double>(timings.frontend.checking.count());
    state.counters["typed_ir_ns"] += static_cast<double>(timings.frontend.ir.count());
    state.counters["code_ns"] += static_cast<double>(timings.code.count());
    state.counters["pause_ns"] += static_cast<double>(timings.pause.count());
    state.counters["llvm_lowering_ns"] += static_cast<double>(timings.native.generation.count());
    state.counters["llvm_optimization_ns"] +=
        static_cast<double>(timings.native.optimization.count());
    state.counters["machine_compilation_ns"] +=
        static_cast<double>(timings.native.compilation.count());
    state.counters["linking_ns"] += static_cast<double>(timings.native.linking.count());
    state.counters["compiled_functions"] += static_cast<double>(result.changes.compiled_functions);
    session.isolate().heap().Collect();
    if (session.code_resources().images != 1) {
      state.SkipWithError("incremental updates retained an unreferenced source image");
      return;
    }
  }
  for (auto& [name, counter] : state.counters) {
    static_cast<void>(name);
    counter /= static_cast<double>(state.iterations());
  }
  state.counters["files"] = static_cast<double>(files);
  state.counters["affected_functions"] = static_cast<double>(kExpectedCompiled);
  state.counters["released_code_images"] = static_cast<double>(session.code_resources().images);
}

int Register() {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    const auto* const prefix =
        tier == NativeCodeTier::Baseline ? "hot/scaling/baseline/" : "hot/scaling/optimized/";
    benchmark::RegisterBenchmark(std::string(prefix) + "independent", ReloadModules, tier,
                                 ModuleGraph::Independent)
        ->Arg(1)
        ->Arg(kMediumProjectFiles)
        ->Arg(kLargeProjectFiles)
        ->Arg(kMaximumProjectFiles)
        ->UseManualTime();
    benchmark::RegisterBenchmark(std::string(prefix) + "imports", ReloadModules, tier,
                                 ModuleGraph::Imports)
        ->Arg(2)
        ->Arg(kMediumProjectFiles)
        ->Arg(kLargeProjectFiles)
        ->Arg(kMaximumProjectFiles)
        ->UseManualTime();
    benchmark::RegisterBenchmark(std::string(prefix) + "fanout", ReloadModules, tier,
                                 ModuleGraph::Fanout)
        ->Arg(2)
        ->Arg(kMediumProjectFiles)
        ->Arg(kLargeProjectFiles)
        ->UseManualTime();
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = Register();

}  // namespace
}  // namespace vela::benchmarks
