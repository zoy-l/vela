#include "compiler/native/native_roots.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_values.h"

namespace vela::compiler::native {
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

void PlanSafepoints(NativeRootPlan& plan, const ir::Function& function) {
  const auto& instructions = function.instructions;
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const auto& instruction = instructions[index];
    const auto backward =
        (instruction.opcode == Opcode::Jump || instruction.opcode == Opcode::JumpIfFalse ||
         instruction.opcode == Opcode::JumpIfNull) &&
        instruction.index <= index;
    if (index == 0 || backward || instruction.opcode == Opcode::Return ||
        ir::AnalyzeEffects(instruction).MayCollectOnSuccess()) {
      plan.safepoints[index] = 1;
    }
    if (index + 1 < instructions.size() && ir::AnalyzeEffects(instruction).MayCollectOnSuccess()) {
      plan.safepoints[index + 1] = 1;
    }
  }
  for (const auto& handler : function.handlers) {
    plan.safepoints[handler.target] = 1;
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
  saved_live.Erase(instruction.dst);
  if (instruction.handler) {
    const auto target = function.handlers.at(*instruction.handler).target;
    const auto exceptional_block = blocks.subspan(graph.instruction_blocks.at(target));
    const auto& exceptional = exceptional_block.front().live_in;
    saved_live.UnionWith(exceptional);
  }
  auto& saved = plan.suspended.at(index);
  saved.clear();
  saved_live.ForEach([&](ir::RegisterId reg) { saved.push_back(reg); });
}

void PlanRematerialization(NativeRootPlan& plan,
                           const ir::Function& function,
                           std::span<const ir::Function> functions) {
  std::vector<std::size_t> definitions(function.register_count);
  std::vector<std::size_t> positions(function.register_count);
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions.at(index);
    if (ir::AnalyzeRegisters(instruction, functions).defines_value) {
      ++definitions.at(instruction.dst);
      positions.at(instruction.dst) = index;
    }
  }
  for (std::size_t index = 0; index < plan.suspended.size(); ++index) {
    std::erase_if(plan.suspended.at(index), [&](ir::RegisterId reg) {
      if (reg < function.parameter_count || definitions.at(reg) != 1) {
        return false;
      }
      const auto position = positions.at(reg);
      const auto opcode = function.instructions.at(position).opcode;
      if (opcode != Opcode::LoadInt && opcode != Opcode::LoadDouble && opcode != Opcode::LoadBool &&
          opcode != Opcode::LoadNull) {
        return false;
      }
      plan.rematerialized.at(index).push_back(position);
      return true;
    });
  }
}

}  // namespace

NativeRootPlan PlanNativeRoots(const ir::Function& function,
                               std::span<const ir::Function> functions,
                               NativeProgramMode mode) {
  NativeRootPlan plan;
  plan.slots.resize(function.register_count);
  plan.stores.resize(function.instructions.size());
  plan.clears.resize(function.instructions.size());
  plan.suspended.resize(function.instructions.size());
  plan.rematerialized.resize(function.instructions.size());
  plan.safepoints.resize(function.instructions.size());
  if (mode == NativeProgramMode::Reloadable) {
    PlanSafepoints(plan, function);
  }
  if (!function.native_symbol.empty()) {
    AssignImportRoots(plan, function);
    return plan;
  }
  const auto graph = ir::BuildFlowGraph(function);
  const auto blocks = ir::AnalyzeLiveness(function, graph, functions);
  ir::RegisterSet after(function.register_count);
  for (std::size_t block_index = 0; block_index < graph.blocks.size(); ++block_index) {
    const auto& block = graph.blocks.at(block_index);
    auto live = blocks.at(block_index).live_out;
    for (auto position = block.end; position > block.begin;) {
      const auto index = --position;
      const auto& instruction = function.instructions.at(index);
      const auto access = ir::AnalyzeRegisters(instruction, functions);
      after = live;
      RecordSuspension(plan, function, graph, blocks, index, after);
      ir::TransferLiveness(function, graph, blocks, instruction, access, live);
      auto& stores = plan.stores.at(index);
      auto& clears = plan.clears.at(index);
      const auto safepoint =
          plan.safepoints[index] != 0 || ir::AnalyzeEffects(instruction).MayCollect();
      live.ForEach([&](ir::RegisterId reg) {
        if (!HasManagedValue(function, reg)) {
          return;
        }
        if (safepoint) {
          stores.push_back(reg);
        }
        if (!after.Has(reg)) {
          clears.push_back(reg);
        }
      });
      if (access.defines_value && HasManagedValue(function, instruction.dst)) {
        clears.push_back(instruction.dst);
      }
      std::ranges::sort(clears);
      const auto duplicate = std::ranges::unique(clears);
      clears.erase(duplicate.begin(), duplicate.end());
    }
  }
  AssignRootSlots(plan);
  PlanRematerialization(plan, function, functions);
  return plan;
}

}  // namespace vela::compiler::native
