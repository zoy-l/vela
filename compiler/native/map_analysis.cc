#include "compiler/native/map_analysis.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "base/builtin.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {
namespace {

struct LookupState final {
  bool reached = false;
  std::optional<std::size_t> lookup;
  bool operator==(const LookupState&) const = default;
};

LookupState Transfer(const ir::Function& function, const ir::BasicBlock& block, LookupState state) {
  for (auto index = block.begin; index < block.end; ++index) {
    const auto& instruction = function.instructions[index];
    if (ir::IsBuiltinCall(instruction, Builtin::MapGet)) {
      state.lookup = index;
    } else {
      const auto effects = ir::AnalyzeEffects(instruction);
      if (effects.reads_state || effects.writes_state || effects.MayCollectOnSuccess()) {
        state.lookup.reset();
      }
    }
  }
  return state;
}

bool IsSameLookup(const ir::Instruction& get,
                  const ir::Instruction& set,
                  std::span<const std::optional<ir::RegisterId>> origins) {
  const auto map = origins[get.lhs];
  const auto key = origins[get.lhs + 1];
  return map && key && map == origins[set.lhs] && key == origins[set.lhs + 1];
}

LookupState MergeLookupPredecessors(const ir::BasicBlock& block,
                                    std::span<const LookupState> exits,
                                    std::size_t block_index) {
  LookupState entry{.reached = block_index == 0, .lookup = std::nullopt};
  for (const auto predecessor : block.predecessors) {
    const auto& previous = exits[predecessor];
    if (!previous.reached) {
      continue;
    }
    if (!entry.reached) {
      entry = previous;
    } else if (entry.lookup != previous.lookup) {
      entry.lookup.reset();
    }
  }
  return entry;
}

}  // namespace

std::vector<std::optional<std::size_t>> FindMapLookups(const ir::Function& function,
                                                       std::span<const ir::Function> functions) {
  std::vector<std::optional<std::size_t>> result(function.instructions.size());
  if (function.instructions.empty() || !ir::CanAnalyzeSynchronousFunction(function)) {
    return result;
  }
  const auto accesses = ir::AnalyzeRegisterAccesses(function, functions);
  const auto register_flow = ir::AnalyzeRegisterFlow(function, accesses);
  const auto graph = ir::BuildFlowGraph(function);
  std::vector<LookupState> entries(graph.blocks.size());
  std::vector<LookupState> exits(graph.blocks.size());
  std::vector<std::size_t> pending{0};
  std::vector<bool> queued(graph.blocks.size());
  queued[0] = true;
  while (!pending.empty()) {
    const auto index = pending.back();
    pending.pop_back();
    queued[index] = false;
    const auto entry = MergeLookupPredecessors(graph.blocks[index], exits, index);
    entries[index] = entry;
    const auto exit = Transfer(function, graph.blocks[index], entry);
    if (exit == exits[index]) {
      continue;
    }
    exits[index] = exit;
    for (const auto successor : graph.blocks[index].successors) {
      if (!queued[successor]) {
        queued[successor] = true;
        pending.push_back(successor);
      }
    }
  }
  for (std::size_t index = 0; index < graph.blocks.size(); ++index) {
    auto state = entries[index];
    if (!state.reached) {
      continue;
    }
    const auto& block = graph.blocks[index];
    for (auto position = block.begin; position < block.end; ++position) {
      const auto& instruction = function.instructions[position];
      if (ir::IsBuiltinCall(instruction, Builtin::MapSet) && state.lookup &&
          IsSameLookup(function.instructions[*state.lookup], instruction, register_flow.origins)) {
        result[position] = state.lookup;
      }
      state = Transfer(
          function, {.begin = position, .end = position + 1, .successors = {}, .predecessors = {}},
          state);
    }
  }
  return result;
}

}  // namespace vela::compiler::native
