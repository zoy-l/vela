#include <cstdint>
#include <filesystem>
#include <sstream>

#include "benchmark/benchmark.h"

#include "compiler/repl_session.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"

namespace vela::benchmarks {
namespace {

constexpr std::int64_t kSessionSamples = 8;

void SessionSubmission(benchmark::State& state) {
  const auto history = state.range(0);
  for (auto iteration : state) {
    state.PauseTiming();
    {
      compiler::ReplSession session(std::filesystem::path("."));
      std::istringstream input;
      std::ostringstream output;
      session.isolate().SetConsole(input, output);
      if (session.Submit("let value = 0;").status != compiler::SubmissionStatus::Executed) {
        state.SkipWithError("cannot create session global");
        break;
      }
      for (std::int64_t index = 0; index < history; ++index) {
        if (session.Submit("value = value + 1;").status != compiler::SubmissionStatus::Executed) {
          state.SkipWithError("cannot populate session history");
          return;
        }
      }
      session.isolate().heap().Collect();
      state.ResumeTiming();
      auto result = session.Submit("value = value + 1;");
      benchmark::DoNotOptimize(result);
      state.PauseTiming();
      if (result.status != compiler::SubmissionStatus::Executed) {
        state.SkipWithError("session submission failed");
        break;
      }
      session.isolate().heap().Collect();
      const auto& heap = session.isolate().heap();
      state.counters["functions"] = static_cast<double>(session.function_count());
      state.counters["source_bytes"] = static_cast<double>(session.source().size());
      state.counters["heap_live_bytes"] = static_cast<double>(heap.allocated_bytes());
      state.counters["heap_committed_bytes"] = static_cast<double>(heap.committed_bytes());
      state.counters["class_layouts"] =
          static_cast<double>(session.isolate().layout_counts().classes);
    }
    state.ResumeTiming();
  }
}

BENCHMARK(SessionSubmission)
    ->Name("repl/submission")
    ->Arg(0)
    ->Arg(32)
    ->Arg(128)
    ->Arg(512)
    ->Iterations(kSessionSamples);

}  // namespace
}  // namespace vela::benchmarks
