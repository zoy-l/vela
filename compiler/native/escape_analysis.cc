#include "compiler/native/escape_analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/escape_dataflow.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/register_set.h"

namespace vela::compiler::native {
namespace internal {

// 预算限制优化工作量，超出预算的合法程序仍使用普通实例存储。
constexpr std::size_t kMaximumObjects = std::numeric_limits<std::uint64_t>::digits;
constexpr std::size_t kMaximumStateCells = std::size_t{1024} * 1024;

std::uint64_t MayHeap(const State& state) {
  return state.at(state.size() - 2).objects;
}

std::uint64_t MustHeap(const State& state) {
  return state.back().objects;
}

bool IsVirtual(const EscapePlan& plan, std::size_t object) {
  return plan.allocations.at(object).decision == AllocationDecision::ScalarReplacement;
}

std::uint64_t Reachable(const EscapePlan& plan, const State& state, std::uint64_t roots) {
  auto reached = roots;
  auto pending = roots;
  while (pending != 0) {
    const auto object = static_cast<std::size_t>(std::countr_zero(pending));
    pending &= pending - 1;
    if (!IsVirtual(plan, object)) {
      continue;
    }
    const auto& allocation = plan.allocations.at(object);
    const auto count = allocation.fields.size();
    for (std::size_t field = 0; field < count; ++field) {
      const auto children = state.at(allocation.state_offset + field).objects & ~reached;
      pending |= children;
      reached |= children;
    }
  }
  return reached;
}

void Publish(const EscapePlan& plan, State& state, std::uint64_t roots) {
  const auto objects = Reachable(plan, state, roots);
  state.at(state.size() - 2).objects |= objects;
  state.back().objects |= objects;
}

std::optional<std::size_t> Object(const Provenance& value) {
  if (value.other || !std::has_single_bit(value.objects)) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(std::countr_zero(value.objects));
}

std::optional<std::size_t> VirtualReceiver(const EscapePlan& plan,
                                           const ir::Instruction& instruction,
                                           const State& values) {
  const auto reg = instruction.opcode == Opcode::SetField ? instruction.dst : instruction.lhs;
  const auto receiver = Object(values.at(reg));
  if (!receiver || !IsVirtual(plan, *receiver)) {
    return std::nullopt;
  }
  const auto bit = std::uint64_t{1} << *receiver;
  if ((MayHeap(values) & bit) == 0) {
    return receiver;
  }
  if ((MustHeap(values) & bit) != 0 ||
      (instruction.opcode != Opcode::GetField && instruction.opcode != Opcode::SetField)) {
    return std::nullopt;
  }
  const auto field = plan.allocations.at(*receiver).state_offset + instruction.index;
  return values.at(field).objects == 0 &&
                 (instruction.opcode == Opcode::GetField || values.at(instruction.lhs).objects == 0)
             ? receiver
             : std::nullopt;
}

bool MatchesType(const ir::Program& program,
                 const AllocationSite& allocation,
                 std::uint32_t type_index) {
  const auto& type = program.layouts.types.at(type_index);
  const auto class_index = allocation.type;
  if (type.kind == layout::Type::Kind::Dynamic) {
    return true;
  }
  if (type.kind == layout::Type::Kind::Instance) {
    return type.index == class_index;
  }
  const auto& interfaces = program.classes.at(class_index).interfaces;
  return type.kind == layout::Type::Kind::Interface &&
         std::ranges::find(interfaces, type.index) != interfaces.end();
}

bool IsTrivialConstructor(const ir::Function& function) {
  if (!function.is_generated_constructor || function.parameter_count != 1 || function.is_async ||
      !function.native_symbol.empty() || !function.handlers.empty()) {
    return false;
  }
  return std::ranges::all_of(function.instructions, [&](const auto& instruction) {
    return instruction.opcode == Opcode::Return ||
           (instruction.opcode == Opcode::LoadNull && instruction.dst >= function.parameter_count);
  });
}

std::optional<std::size_t> BorrowedReceiver(const EscapePlan& plan,
                                            const ir::Instruction& instruction,
                                            const State& values) {
  if (instruction.opcode != Opcode::Call || plan.borrowable.empty()) {
    return std::nullopt;
  }
  const auto type = plan.borrowable.subspan(instruction.index).front();
  const auto object = ScalarObject(plan, values.at(instruction.lhs));
  if (!type || !object || (MayHeap(values) & (std::uint64_t{1} << *object)) != 0) {
    return std::nullopt;
  }
  const auto& site = plan.allocations.at(*object);
  if (site.type != *type) {
    return std::nullopt;
  }
  for (std::size_t field = 0; field < site.fields.size(); ++field) {
    const auto& value = values.at(site.state_offset + field);
    if (!value.initialized || value.objects != 0) {
      return std::nullopt;
    }
  }
  return object;
}

bool NeedsHeap(const EscapePlan& plan,
               const ir::Program& program,
               const ir::Instruction& instruction,
               const State& values) {
  switch (instruction.opcode) {
    case Opcode::Move:
    case Opcode::JumpIfNull:
    case Opcode::IsType:
      return false;
    case Opcode::CheckedCast: {
      const auto object = Object(values.at(instruction.lhs));
      return !object || !MatchesType(program, plan.allocations.at(*object), instruction.index);
    }
    case Opcode::Call:
      return !IsTrivialConstructor(program.functions.at(instruction.index)) &&
             !BorrowedReceiver(plan, instruction, values);
    case Opcode::GetField:
    case Opcode::SetField:
      return !VirtualReceiver(plan, instruction, values);
    case Opcode::EqualValue:
    case Opcode::NotEqualValue:
      return !Object(values.at(instruction.lhs)) || !Object(values.at(instruction.rhs));
    default:
      return true;
  }
}

bool Merge(BlockState& destination, const State& source) {
  if (!destination.reached) {
    destination.values = source;
    destination.reached = true;
    return true;
  }
  bool changed = false;
  for (std::size_t reg = 0; reg < source.size(); ++reg) {
    auto& value = destination.values.at(reg);
    const auto& incoming = source.at(reg);
    const Provenance merged{
        .objects = reg + 1 == source.size() ? value.objects & incoming.objects
                                            : value.objects | incoming.objects,
        .other = value.other || incoming.other,
        .initialized = value.initialized && incoming.initialized,
    };
    changed = changed || value != merged;
    value = merged;
  }
  return changed;
}

void Transfer(const ir::Instruction& instruction,
              const ir::RegisterAccess& access,
              std::optional<std::size_t> allocation,
              const EscapePlan& plan,
              const ir::Program& program,
              State& values) {
  const auto receiver = VirtualReceiver(plan, instruction, values);
  const bool virtual_receiver = receiver.has_value();
  if (instruction.opcode == Opcode::SetField && virtual_receiver) {
    const auto field = plan.allocations.at(*receiver).state_offset + instruction.index;
    values.at(field) = values.at(instruction.lhs);
    values.at(field).initialized = true;
    return;
  }
  if (NeedsHeap(plan, program, instruction, values)) {
    ir::ForEachUse(instruction, access,
                   [&](ir::RegisterId reg) { Publish(plan, values, values.at(reg).objects); });
  }
  if (!access.defines_value) {
    return;
  }
  Provenance value;
  if (allocation && *allocation < kMaximumObjects) {
    value = {.objects = std::uint64_t{1} << *allocation, .other = false};
    values.at(values.size() - 2).objects &= ~value.objects;
    values.back().objects &= ~value.objects;
    const auto& object = plan.allocations.at(*allocation);
    for (std::size_t field = 0; field < object.fields.size(); ++field) {
      values.at(object.state_offset + field) = {};
    }
    if (!IsVirtual(plan, *allocation)) {
      Publish(plan, values, value.objects);
    }
  } else if (instruction.opcode == Opcode::Move ||
             (instruction.opcode == Opcode::CheckedCast &&
              !NeedsHeap(plan, program, instruction, values))) {
    value = values.at(instruction.lhs);
  } else if (instruction.opcode == Opcode::GetField && virtual_receiver) {
    value = values.at(plan.allocations.at(*receiver).state_offset + instruction.index);
  }
  values.at(instruction.dst) = value;
}

State BeforeFailure(const EscapePlan& plan,
                    const ir::Program& program,
                    const ir::Instruction& instruction,
                    const State& values) {
  auto exceptional = values;
  if (NeedsHeap(plan, program, instruction, values)) {
    std::uint64_t published = 0;
    ir::ForEachUse(instruction, ir::AnalyzeRegisters(instruction, program.functions),
                   [&](ir::RegisterId reg) { published |= values.at(reg).objects; });
    // 物化可能先失败，也可能调用已进入并修改对象后才抛错。
    // 异常合流必须保留这两种表示，不能假定失败意味着尚未物化。
    exceptional.at(exceptional.size() - 2).objects |= Reachable(plan, values, published);
  }
  return exceptional;
}

std::vector<BlockState> Solve(const ir::Function& function,
                              const ir::Program& program,
                              const ir::FlowGraph& graph,
                              std::span<const std::optional<std::size_t>> allocations,
                              const EscapePlan& plan,
                              std::size_t state_size,
                              State entry) {
  std::vector<BlockState> states(graph.blocks.size());
  if (states.empty()) {
    return states;
  }
  states.front() = {
      .values = entry.empty() ? State(state_size) : std::move(entry),
      .reached = true,
  };
  std::deque<std::size_t> pending{0};
  std::vector<std::uint8_t> queued(states.size());
  queued.front() = 1;
  while (!pending.empty()) {
    const auto index = pending.front();
    pending.pop_front();
    queued.at(index) = 0;
    auto values = states.at(index).values;
    const auto& block = graph.blocks.at(index);
    std::optional<State> exceptional;
    std::optional<std::size_t> handler;
    for (auto position = block.begin; position < block.end; ++position) {
      const auto& instruction = function.instructions.at(position);
      if (instruction.handler && ir::AnalyzeEffects(instruction).may_fail) {
        exceptional = BeforeFailure(plan, program, instruction, values);
        handler = graph.instruction_blocks.at(function.handlers.at(*instruction.handler).target);
      }
      Transfer(instruction, ir::AnalyzeRegisters(instruction, program.functions),
               allocations.subspan(position).front(), plan, program, values);
    }
    const auto propagate = [&](std::size_t successor, const State& incoming) {
      if (Merge(states.at(successor), incoming) && queued.at(successor) == 0) {
        queued.at(successor) = 1;
        pending.push_back(successor);
      }
    };
    const auto& last = function.instructions.at(block.end - 1);
    // 同一块可以同时以正常边和异常边进入同一 handler，分别合并两种定义状态。
    if (last.opcode == Opcode::Jump || last.opcode == Opcode::JumpIfFalse ||
        last.opcode == Opcode::JumpIfNull) {
      if (last.index < function.instructions.size()) {
        propagate(graph.instruction_blocks.at(last.index), values);
      }
    }
    if (last.opcode != Opcode::Jump && last.opcode != Opcode::Return &&
        last.opcode != Opcode::Throw && last.opcode != Opcode::Rethrow &&
        index + 1 < states.size()) {
      propagate(index + 1, values);
    }
    if (handler) {
      propagate(*handler, *exceptional);
    }
  }
  return states;
}

void Exclude(EscapePlan& plan, std::uint64_t objects, AllocationDecision reason) {
  while (objects != 0) {
    const auto index = static_cast<std::size_t>(std::countr_zero(objects));
    auto& decision = plan.allocations.at(index).decision;
    if (decision == AllocationDecision::ScalarReplacement) {
      decision = reason;
    }
    objects &= objects - 1;
  }
}

void InspectProvenance(EscapePlan& plan, const Provenance& value) {
  if (value.objects != 0 && !Object(value)) {
    Exclude(plan, value.objects, AllocationDecision::AmbiguousAlias);
  }
}

std::vector<std::vector<ir::RegisterId>> LiveBefore(const ir::Function& function,
                                                    const ir::Program& program,
                                                    const ir::FlowGraph& graph) {
  const auto blocks = ir::AnalyzeLiveness(function, graph, program.functions);
  std::vector<std::vector<ir::RegisterId>> result(function.instructions.size());
  for (std::size_t index = 0; index < graph.blocks.size(); ++index) {
    const auto& block = graph.blocks.at(index);
    auto live = blocks.at(index).live_out;
    for (auto position = block.end; position > block.begin;) {
      const auto& instruction = function.instructions.at(--position);
      ir::TransferLiveness(function, graph, blocks, instruction,
                           ir::AnalyzeRegisters(instruction, program.functions), live);
      live.ForEach([&](ir::RegisterId reg) { result.at(position).push_back(reg); });
    }
  }
  return result;
}

void CheckMaterialization(EscapePlan& plan, std::size_t root, const State& values) {
  const auto root_bit = std::uint64_t{1} << root;
  if (!IsVirtual(plan, root) || (MustHeap(values) & root_bit) != 0) {
    return;
  }
  auto pending = Reachable(plan, values, root_bit);
  while (pending != 0) {
    const auto node = static_cast<std::size_t>(std::countr_zero(pending));
    pending &= pending - 1;
    if (!IsVirtual(plan, node)) {
      continue;
    }
    const auto& allocation = plan.allocations.at(node);
    if (node != root && (MayHeap(values) & (std::uint64_t{1} << node)) != 0) {
      Exclude(plan, root_bit, AllocationDecision::UnsupportedUse);
    }
    for (std::size_t field = 0; field < allocation.fields.size(); ++field) {
      const auto& value = values.at(allocation.state_offset + field);
      if (value.objects != 0 && !Object(value)) {
        Exclude(plan, value.objects, AllocationDecision::AmbiguousAlias);
      }
    }
  }
}

void InspectRecovery(EscapePlan& plan,
                     const ir::Instruction& instruction,
                     const State& values,
                     std::span<const ir::RegisterId> live) {
  if (instruction.opcode == Opcode::GetField) {
    const auto receiver = VirtualReceiver(plan, instruction, values);
    if (receiver &&
        !values.at(plan.allocations.at(*receiver).state_offset + instruction.index).initialized) {
      Exclude(plan, std::uint64_t{1} << *receiver, AllocationDecision::UnsupportedUse);
    }
  }
  for (const auto reg : live) {
    if (const auto root = Object(values.at(reg))) {
      CheckMaterialization(plan, *root, values);
    }
  }
}

void InspectBoundary(EscapePlan& plan,
                     const ir::Program& program,
                     const ir::Instruction& instruction,
                     const State& values) {
  const auto access = ir::AnalyzeRegisters(instruction, program.functions);
  if (instruction.handler && !ir::AnalyzeEffects(instruction).may_fail &&
      NeedsHeap(plan, program, instruction, values)) {
    ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) {
      Exclude(plan, values.at(reg).objects, AllocationDecision::UnsupportedUse);
    });
  }
  if (instruction.opcode == Opcode::Call) {
    const auto& callee = program.functions.at(instruction.index);
    if (callee.name.ends_with(".$constructor") && !IsTrivialConstructor(callee)) {
      ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) {
        Exclude(plan, values.at(reg).objects, AllocationDecision::CallBoundary);
      });
    }
  }
}

void InspectLifetime(EscapePlan& plan,
                     const ir::Instruction& instruction,
                     std::optional<std::size_t> allocation,
                     const State& values,
                     std::span<const ir::RegisterId> live) {
  std::uint64_t live_objects = 0;
  for (const auto reg : live) {
    live_objects |= values.at(reg).objects;
  }
  if (instruction.opcode == Opcode::Await) {
    Exclude(plan, live_objects, AllocationDecision::Publication);
  }
  if (allocation && *allocation < kMaximumObjects) {
    const auto object = std::uint64_t{1} << *allocation;
    if ((Reachable(plan, values, live_objects) & object) != 0) {
      Exclude(plan, object, AllocationDecision::LoopLifetime);
    }
  }
}

void InspectInstruction(EscapePlan& plan,
                        const ir::Program& program,
                        const ir::Instruction& instruction,
                        std::optional<std::size_t> allocation,
                        const State& values,
                        std::span<const ir::RegisterId> live) {
  if (program.native_mode == NativeProgramMode::Reloadable) {
    InspectRecovery(plan, instruction, values, live);
  }
  InspectBoundary(plan, program, instruction, values);
  InspectLifetime(plan, instruction, allocation, values, live);
  const auto access = ir::AnalyzeRegisters(instruction, program.functions);
  ir::ForEachUse(instruction, access,
                 [&](ir::RegisterId reg) { InspectProvenance(plan, values.at(reg)); });
  if (NeedsHeap(plan, program, instruction, values)) {
    ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) {
      if (const auto root = Object(values.at(reg))) {
        CheckMaterialization(plan, *root, values);
      }
    });
  }
}

void Inspect(EscapePlan& plan,
             const ir::Program& program,
             const ir::FlowGraph& graph,
             std::span<const BlockState> blocks,
             std::span<const std::optional<std::size_t>> allocations,
             std::span<const std::vector<ir::RegisterId>> live) {
  const auto& function = plan.function;
  for (std::size_t index = 0; index < graph.blocks.size(); ++index) {
    const auto& state = blocks.subspan(index).front();
    if (!state.reached) {
      continue;
    }
    auto values = state.values;
    const auto& block = graph.blocks.at(index);
    for (auto position = block.begin; position < block.end; ++position) {
      const auto& instruction = function.instructions.at(position);
      const auto allocation = allocations.subspan(position).front();
      InspectInstruction(plan, program, instruction, allocation, values,
                         live.subspan(position).front());
      Transfer(instruction, ir::AnalyzeRegisters(instruction, program.functions), allocation, plan,
               program, values);
    }
  }
}

AllocationDecision InitialDecision(const ir::Program& program,
                                   const ir::Instruction& instruction,
                                   std::size_t count,
                                   EscapeOptimization optimization,
                                   NativeCodeTier tier) {
  if (optimization == EscapeOptimization::Disabled) {
    return AllocationDecision::OptimizationDisabled;
  }
  if (program.native_mode == NativeProgramMode::Reloadable && tier != NativeCodeTier::Optimized) {
    return AllocationDecision::RecoveryRequired;
  }
  if (count >= kMaximumObjects) {
    return AllocationDecision::AnalysisBudget;
  }
  const auto& type = program.classes.at(instruction.index);
  return type.kind != ir::Class::Kind::Instance || type.callable
             ? AllocationDecision::NonInstance
             : AllocationDecision::ScalarReplacement;
}

std::size_t IndexAllocations(EscapePlan& plan,
                             const ir::Program& program,
                             EscapeOptimization optimization,
                             NativeCodeTier tier,
                             std::vector<std::optional<std::size_t>>& allocations) {
  const auto& function = plan.function;
  std::size_t state_size = function.register_count;
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions.at(index);
    if (instruction.opcode != Opcode::NewInstance) {
      continue;
    }
    const auto count = plan.allocations.size();
    allocations.at(index) = count;
    const auto field_count = program.classes.at(instruction.index).field_types.size();
    auto decision = InitialDecision(program, instruction, count, optimization, tier);
    if (decision == AllocationDecision::ScalarReplacement &&
        state_size + field_count + 2 > kMaximumStateCells) {
      decision = AllocationDecision::AnalysisBudget;
    }
    const auto field_slots = decision == AllocationDecision::ScalarReplacement ? field_count : 0;
    plan.allocations.push_back({
        .instruction = index,
        .type = instruction.index,
        .decision = decision,
        .fields = std::vector<ir::RegisterId>(field_slots),
        .state_offset = state_size,
    });
    state_size += field_slots;
  }
  return state_size + 2;
}

std::size_t EligibleCount(const EscapePlan& plan) {
  return static_cast<std::size_t>(std::ranges::count_if(plan.allocations, [](const auto& site) {
    return site.decision == AllocationDecision::ScalarReplacement;
  }));
}

std::optional<std::size_t> ScalarObject(const EscapePlan& plan, const Provenance& value) {
  const auto object = Object(value);
  return object && plan.allocations.at(*object).decision == AllocationDecision::ScalarReplacement
             ? object
             : std::nullopt;
}

}  // namespace internal

using namespace internal;

EscapePlan PlanEscapes(const ir::Program& program,
                       std::size_t function_index,
                       EscapeOptimization optimization,
                       NativeCodeTier tier,
                       std::span<const std::optional<ir::ClassId>> borrowable) {
  EscapePlan plan{
      .function = program.functions.at(function_index),
      .origins = {},
      .allocations = {},
      .checkpoints = {},
      .borrowable = {},
      .borrowed_calls = {},
      .borrowed_fields = {},
  };
  if (optimization == EscapeOptimization::Enabled &&
      program.native_mode == NativeProgramMode::Closed) {
    plan.borrowable = borrowable;
  }
  const auto& function = plan.function;
  std::vector<std::optional<std::size_t>> allocations(function.instructions.size());
  const auto state_size = IndexAllocations(plan, program, optimization, tier, allocations);
  const auto preserve = [&] {
    plan.borrowable = {};
    for (std::size_t index = 0; index < function.instructions.size(); ++index) {
      plan.origins.push_back(index);
    }
  };
  if (EligibleCount(plan) == 0) {
    preserve();
    return plan;
  }
  const auto graph = ir::BuildFlowGraph(function);
  const auto state_cells = static_cast<std::uint64_t>(state_size) * graph.blocks.size();
  if (state_cells > kMaximumStateCells) {
    for (auto& allocation : plan.allocations) {
      allocation.decision = AllocationDecision::AnalysisBudget;
    }
    preserve();
    return plan;
  }
  const auto live = LiveBefore(function, program, graph);
  std::vector<BlockState> states;
  auto eligible = EligibleCount(plan);
  while (eligible != 0) {
    const auto previous_count = eligible;
    states = Solve(function, program, graph, allocations, plan, state_size);
    Inspect(plan, program, graph, states, allocations, live);
    eligible = EligibleCount(plan);
    if (eligible == previous_count) {
      RewriteEscapes(plan, program, live, graph, states, allocations);
      plan.borrowable = {};
      return plan;
    }
  }
  preserve();
  return plan;
}

}  // namespace vela::compiler::native
