#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>

#include "benchmark/benchmark.h"

#include <utility>
#include "base/path.h"
#include "compiler/development/development_repl.h"
#include "compiler/development/development_session.h"
#include "compiler/repl/repl_source.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"

namespace vela::benchmarks {
namespace {

constexpr std::int64_t kSessionSamples = 8;

void SessionSubmission(benchmark::State& state) {
  const auto history = state.range(0);
  for (auto iteration : state) {
    state.PauseTiming();
    {
      const auto directory = std::filesystem::current_path();
      const auto entry = PathText(directory / "__benchmark_host__.vela");
      auto created = compiler::DevelopmentSession::Create(entry, {});
      if (!created) {
        state.SkipWithError("cannot create REPL development session");
        return;
      }
      auto& session = **created;
      auto candidate = session.Prepare(
          {.changes = {{.path = entry, .text = "func main(): void {}"}}, .added_roots = {}},
          compiler::DevelopmentOperation::Load);
      if (!candidate) {
        state.SkipWithError(candidate.error().message);
        return;
      }
      (void)session.Submit(std::move(*candidate));
      if (session.Pump().status != compiler::DevelopmentStatus::Accepted) {
        state.SkipWithError("cannot load REPL development session");
        return;
      }
      compiler::DevelopmentRepl repl(session, PathText(directory / "__benchmark_repl__.vela"));
      std::ostringstream output;
      session.isolate().SetOutput(output);
      if (repl.Submit("let value = 0;").status != compiler::SubmissionStatus::Executed) {
        state.SkipWithError("cannot create session global");
        break;
      }
      for (std::int64_t index = 0; index < history; ++index) {
        if (repl.Submit("value = value + 1;").status != compiler::SubmissionStatus::Executed) {
          state.SkipWithError("cannot populate session history");
          return;
        }
      }
      session.isolate().heap().Collect();
      state.ResumeTiming();
      auto result = repl.Submit("value = value + 1;");
      benchmark::DoNotOptimize(result);
      state.PauseTiming();
      if (result.status != compiler::SubmissionStatus::Executed) {
        state.SkipWithError("session submission failed");
        break;
      }
      session.isolate().heap().Collect();
      const auto& heap = session.isolate().heap();
      state.counters["functions"] =
          static_cast<double>(session.snapshot()->program.functions.size());
      state.counters["source_bytes"] = static_cast<double>(repl.source().size());
      state.counters["heap_live_bytes"] = static_cast<double>(heap.allocated_bytes());
      state.counters["heap_committed_bytes"] = static_cast<double>(heap.committed_bytes());
      state.counters["class_layouts"] =
          static_cast<double>(session.isolate().layout_counts().classes);
    }
    state.ResumeTiming();
  }
}

void PrepareSource(benchmark::State& state) {
  constexpr std::uint32_t kHistoryDeclarations = 128;
  compiler::ReplSource source;
  for (std::uint32_t index = 0; index < kHistoryDeclarations; ++index) {
    auto candidate = source.Prepare("const previous" + std::to_string(index) + " = 1;");
    if (!candidate) {
      state.SkipWithError("cannot populate REPL declaration history");
      return;
    }
    source.Commit(*candidate);
  }
  std::string submission = "func submitted(): int { let value = 0;";
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    submission += "value = value + 1;";
  }
  submission += "return value; }";
  for (auto iteration : state) {
    auto candidate = source.Prepare(submission);
    if (!candidate || candidate->entry.submission != kHistoryDeclarations + 1 ||
        candidate->parsed.program.globals.size() != kHistoryDeclarations ||
        candidate->parsed.program.functions.size() != 2) {
      state.SkipWithError("REPL candidate lost its independent declarations or entry");
      return;
    }
    benchmark::DoNotOptimize(candidate);
  }
}

BENCHMARK(PrepareSource)->Name("repl/source_prepare")->Arg(256);

BENCHMARK(SessionSubmission)
    ->Name("repl/submission")
    ->Arg(0)
    ->Arg(32)
    ->Arg(128)
    ->Arg(512)
    ->Iterations(kSessionSamples);

}  // namespace
}  // namespace vela::benchmarks
