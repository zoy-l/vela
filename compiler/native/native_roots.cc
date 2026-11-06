#include "compiler/native/native_roots.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <queue>
#include <span>
#include <utility>
#include <vector>

#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/register_set.h"
#include "compiler/native/native_values.h"

namespace vela::compiler::native {
namespace {

bool HasManagedValue(const ir::Function& function, ir::RegisterId reg) {
  const auto kind = function.value_types.at(reg).kind;
  return kind == Kind::Dynamic || IsReferenceKind(kind);
}

struct RootLifetime final {
  ir::RegisterId reg = 0;
  std::size_t begin = std::numeric_limits<std::size_t>::max();
  std::size_t end = 0;

  void Include(std::size_t instruction) {
    begin = std::min(begin, instruction);
    end = std::max(end, instruction);
  }
};

void AssignRootSlots(NativeRootPlan& plan, std::span<const RootLifetime> lifetimes) {
  ir::RegisterSet candidates(plan.slots.size());
  for (const auto& stores : plan.stores) {
    for (const auto reg : stores) {
      candidates.Insert(reg);
    }
  }
  std::vector<RootLifetime> ordered;
  candidates.ForEach([&](ir::RegisterId reg) {
    auto lifetime = lifetimes[reg];
    lifetime.reg = reg;
    ordered.push_back(lifetime);
  });
  std::ranges::sort(ordered, [](const auto& left, const auto& right) {
    return left.begin != right.begin ? left.begin < right.begin : left.reg < right.reg;
  });
  using SlotEnd = std::pair<std::size_t, std::size_t>;
  std::priority_queue<SlotEnd, std::vector<SlotEnd>, std::greater<>> active;
  std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> available;
  for (const auto& lifetime : ordered) {
    // 同一指令的输入、输出和清空点共用边界，不能因成功路径上的定义而覆盖异常活跃值。
    while (!active.empty() && active.top().first < lifetime.begin) {
      available.push(active.top().second);
      active.pop();
    }
    std::size_t slot = 0;
    if (available.empty()) {
      slot = plan.count++;
    } else {
      slot = available.top();
      available.pop();
    }
    plan.slots.at(lifetime.reg) = slot;
    active.emplace(lifetime.end, slot);
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

void PlanSafepoints(NativeRootPlan& plan, const ir::Function& function, NativeDebugging debugging) {
  const auto& instructions = function.instructions;
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const auto& instruction = instructions.at(index);
    const auto backward =
        (instruction.opcode == Opcode::Jump || instruction.opcode == Opcode::JumpIfFalse ||
         instruction.opcode == Opcode::JumpIfNull) &&
        instruction.index <= index;
    const auto source_line =
        debugging == NativeDebugging::SourceLines &&
        (index == 0 || instruction.location.line != instructions.at(index - 1).location.line);
    if (debugging == NativeDebugging::Instructions || index == 0 || backward || source_line ||
        instruction.opcode == Opcode::Return) {
      plan.safepoints.at(index) |= kNativeSafepointBoundary;
    }
    if (ir::AnalyzeEffects(instruction).MayCollectOnSuccess()) {
      plan.safepoints.at(index) |= kNativeSafepointBeforeRuntime;
      if (index + 1 < instructions.size()) {
        plan.safepoints.at(index + 1) |= kNativeSafepointAfterRuntime;
      }
    }
    if (instruction.opcode == Opcode::SetField && instruction.field_declaration) {
      plan.safepoints.at(index) |= kNativeSafepointRecovery;
    }
  }
  for (const auto& handler : function.handlers) {
    plan.safepoints.at(handler.target) |= kNativeSafepointBoundary;
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
  if (std::ranges::none_of(plan.suspended, [](const auto& values) { return !values.empty(); })) {
    return;
  }
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

NativeRootPlan InitializeRoots(const ir::Function& function,
                               NativeProgramMode mode,
                               NativeDebugging debugging,
                               NativeRecoveryRequirements recovery) {
  NativeRootPlan plan;
  plan.debug_values = debugging == NativeDebugging::Instructions;
  plan.bounded_polls = debugging == NativeDebugging::Safepoints;
  plan.slots.resize(function.register_count);
  plan.stores.resize(function.instructions.size());
  plan.clears.resize(function.instructions.size());
  plan.suspended.resize(function.instructions.size());
  plan.rematerialized.resize(function.instructions.size());
  plan.safepoints.resize(function.instructions.size());
  plan.checkpoints.resize(function.instructions.size());
  plan.minimum_temporary_count = recovery.minimum_temporary_count;
  if (!recovery.safepoints.empty()) {
    plan.safepoints.assign(recovery.safepoints.begin(), recovery.safepoints.end());
  } else if (mode == NativeProgramMode::Reloadable || plan.debug_values) {
    PlanSafepoints(plan, function, debugging);
  }
  return plan;
}

struct InstructionLiveness final {
  std::reference_wrapper<const ir::RegisterSet> before;
  std::reference_wrapper<const ir::RegisterSet> after;
};

void RecordLiveRoots(NativeRootPlan& plan,
                     const ir::Function& function,
                     std::size_t index,
                     const ir::RegisterAccess& access,
                     InstructionLiveness liveness,
                     std::span<RootLifetime> lifetimes) {
  const auto& instruction = function.instructions[index];
  auto& stores = plan.stores[index];
  auto& clears = plan.clears[index];
  const auto safepoint =
      plan.safepoints[index] != 0 || ir::AnalyzeEffects(instruction).MayCollect();
  const auto& after = liveness.after.get();
  liveness.before.get().ForEach([&](ir::RegisterId reg) {
    if (!HasManagedValue(function, reg)) {
      return;
    }
    lifetimes[reg].Include(index);
    if (safepoint) {
      stores.push_back(reg);
    }
    if (!after.Has(reg)) {
      clears.push_back(reg);
    }
  });
  if (access.defines_value && HasManagedValue(function, instruction.dst)) {
    clears.push_back(instruction.dst);
    // 逆向传递只会从成功后继删除 dst；其余 after 值已包含在 before 中。
    lifetimes[instruction.dst].Include(index);
  }
  std::ranges::sort(clears);
  const auto duplicate = std::ranges::unique(clears);
  clears.erase(duplicate.begin(), duplicate.end());
}

}  // namespace

NativeRootPlan PlanNativeRoots(const ir::Function& function,
                               std::span<const ir::Function> functions,
                               NativeProgramMode mode,
                               NativeDebugging debugging,
                               NativeRecoveryRequirements recovery) {
  auto plan = InitializeRoots(function, mode, debugging, recovery);
  if (!function.native_symbol.empty()) {
    AssignImportRoots(plan, function);
    return plan;
  }
  const auto graph = ir::BuildFlowGraph(function);
  const auto required = recovery.values;
  const auto blocks = ir::AnalyzeLiveness(function, graph, functions, required);
  std::vector<RootLifetime> lifetimes(function.register_count);
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
      ir::TransferLiveness(function, graph, blocks, instruction, access, live,
                           required.empty() ? std::span<const ir::RegisterId>{} : required[index]);
      if (mode == NativeProgramMode::Reloadable || plan.debug_values) {
        live.ForEach([&](ir::RegisterId reg) { plan.checkpoints[index].push_back(reg); });
      }
      RecordLiveRoots(plan, function, index, access, {.before = live, .after = after}, lifetimes);
    }
  }
  AssignRootSlots(plan, lifetimes);
  plan.count = std::max(plan.count, recovery.minimum_root_count);
  PlanRematerialization(plan, function, functions);
  return plan;
}

}  // namespace vela::compiler::native
