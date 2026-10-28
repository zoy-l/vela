#include "compiler/aot/native_roots.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "base/opcode.h"
#include "compiler/aot/native_values.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::aot {
namespace {

bool HasManagedValue(const ir::Function& function, ir::RegisterId reg) {
  const auto kind = function.value_types.at(reg).kind;
  return kind == Kind::Dynamic || IsReferenceKind(kind);
}

void AssignRootSlots(NativeRootPlan& plan) {
  for (const auto& stores : plan.stores) {
    for (const auto reg : stores) {
      auto& slot = plan.slots.at(reg);
      if (!slot) {
        slot = plan.count++;
      }
    }
  }
  for (auto& clears : plan.clears) {
    std::erase_if(clears, [&](ir::RegisterId reg) { return !plan.slots.at(reg).has_value(); });
  }
}

void AssignImportRoots(NativeRootPlan& plan, const ir::Function& function) {
  for (ir::RegisterId reg = 0; reg < function.parameter_count; ++reg) {
    if (HasManagedValue(function, reg)) {
      plan.slots.at(reg) = plan.count++;
    }
  }
}

void RecordSuspension(NativeRootPlan& plan,
                      const ir::Function& function,
                      const ir::FlowGraph& graph,
                      std::span<const ir::BlockLiveness> blocks,
                      std::size_t index,
                      const ir::RegisterSet& live) {
  const auto& instruction = function.instructions.at(index);
  if (instruction.opcode != Opcode::Await) {
    return;
  }
  auto saved_live = live;
  saved_live.erase(instruction.dst);
  if (instruction.handler) {
    const auto target = function.handlers.at(*instruction.handler).target;
    const auto exceptional_block = blocks.subspan(graph.instruction_blocks.at(target));
    const auto& exceptional = exceptional_block.front().live_in;
    saved_live.insert(exceptional.begin(), exceptional.end());
  }
  auto& saved = plan.suspended.at(index);
  saved.assign(saved_live.begin(), saved_live.end());
  std::ranges::sort(saved);
}

}  // namespace

NativeRootPlan PlanNativeRoots(const ir::Function& function,
                               std::span<const ir::Function> functions) {
  NativeRootPlan plan;
  plan.slots.resize(function.register_count);
  plan.stores.resize(function.instructions.size());
  plan.clears.resize(function.instructions.size());
  plan.suspended.resize(function.instructions.size());
  if (!function.native_symbol.empty()) {
    AssignImportRoots(plan, function);
    return plan;
  }
  const auto graph = ir::BuildFlowGraph(function);
  const auto blocks = ir::AnalyzeLiveness(function, graph, functions);
  for (std::size_t block_index = 0; block_index < graph.blocks.size(); ++block_index) {
    const auto& block = graph.blocks.at(block_index);
    auto live = blocks.at(block_index).live_out;
    for (auto position = block.end; position > block.begin;) {
      const auto index = --position;
      const auto& instruction = function.instructions.at(index);
      const auto access = ir::AnalyzeRegisters(instruction, functions);
      const auto after = live;
      RecordSuspension(plan, function, graph, blocks, index, after);
      ir::TransferLiveness(function, graph, blocks, instruction, access, live);
      auto& stores = plan.stores.at(index);
      auto& clears = plan.clears.at(index);
      const auto safepoint = ir::AnalyzeEffects(instruction).MayCollect();
      for (const auto reg : live) {
        if (!HasManagedValue(function, reg)) {
          continue;
        }
        if (safepoint) {
          stores.push_back(reg);
        }
        if (!after.contains(reg)) {
          clears.push_back(reg);
        }
      }
      if (access.defines_value && HasManagedValue(function, instruction.dst)) {
        clears.push_back(instruction.dst);
      }
      std::ranges::sort(stores);
      std::ranges::sort(clears);
      const auto duplicate = std::ranges::unique(clears);
      clears.erase(duplicate.begin(), duplicate.end());
    }
  }
  AssignRootSlots(plan);
  return plan;
}

}  // namespace vela::compiler::aot
