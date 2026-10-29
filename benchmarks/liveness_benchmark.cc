#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::benchmarks {
namespace {

enum class LivenessMode { All, Necessary };

void LoopLiveness(benchmark::State& state, LivenessMode mode) {
  ir::Function function;
  function.register_count = 128;
  const auto blocks = static_cast<std::size_t>(state.range(0));
  const auto width = static_cast<std::size_t>(state.range(1));
  for (std::size_t block = 0; block < blocks; ++block) {
    for (std::size_t index = 0; index < width; ++index) {
      const auto value = block * width + index;
      function.instructions.push_back({
          .opcode = Opcode::Move,
          .dst = 1 + static_cast<std::uint32_t>((value + 1) % 127),
          .lhs = 1 + static_cast<std::uint32_t>(value % 127),
          .location = {},
      });
    }
    function.instructions.push_back({
        .opcode = Opcode::JumpIfFalse,
        .lhs = 0,
        .index = static_cast<std::uint32_t>((block == 0 ? 0 : block - 1) * (width + 1)),
        .location = {},
    });
  }
  function.instructions.push_back({.opcode = Opcode::Return, .dst = 127, .location = {}});
  const auto graph = ir::BuildFlowGraph(function);
  for (auto iteration : state) {
    auto live = mode == LivenessMode::All
                    ? ir::AnalyzeLiveness(function, graph, {})
                    : ir::AnalyzeNecessaryLiveness(function, graph, {}).blocks;
    if (live.empty() || !live.front().live_in.Has(0)) {
      state.SkipWithError("Loop condition must remain live at entry");
      break;
    }
    benchmark::DoNotOptimize(live);
  }
  state.counters["instructions"] = static_cast<double>(function.instructions.size());
  state.counters["blocks"] = static_cast<double>(graph.blocks.size());
}

BENCHMARK_CAPTURE(LoopLiveness, All, LivenessMode::All)
    ->Name("analysis/liveness/all")
    ->Args({8, 16})
    ->Args({32, 64})
    ->Args({64, 256});
BENCHMARK_CAPTURE(LoopLiveness, Necessary, LivenessMode::Necessary)
    ->Name("analysis/liveness/necessary")
    ->Args({8, 16})
    ->Args({32, 64})
    ->Args({64, 256});

}  // namespace
}  // namespace vela::benchmarks
