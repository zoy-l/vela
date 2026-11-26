#include "compiler/native/call_targets.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/SmallVector.h>

#include "base/builtin.h"
#include "base/callable.h"
#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_roots.h"
#include "compiler/native/native_values.h"

namespace vela::compiler::native {
namespace {

void SortUnique(std::vector<std::size_t>& values) {
  std::ranges::sort(values);
  const auto duplicates = std::ranges::unique(values);
  values.erase(duplicates.begin(), duplicates.end());
}

bool CanCallDirectly(const ir::Instruction& instruction,
                     const ir::Function& caller,
                     const ir::Function& callee,
                     CallableMode mode) {
  const bool bound_native = mode == CallableMode::BoundMethod && !callee.native_symbol.empty();
  if (mode != CallableMode::Plain && mode != CallableMode::Closure && !bound_native) {
    return false;
  }
  const auto hidden = mode == CallableMode::Closure || bound_native ? 1U : 0U;
  if (callee.parameter_count != instruction.index + hidden) {
    return false;
  }
  for (std::size_t index = 0; index < instruction.index; ++index) {
    const auto& source = caller.value_types.at(instruction.lhs + index);
    const auto& target = callee.value_types.at(index + hidden);
    if (!IsSameType(source, target)) {
      return false;
    }
  }
  return IsSameType(caller.value_types.at(instruction.dst), callee.return_type);
}

bool CanUseVirtualArguments(const ir::Instruction& instruction,
                            const ir::Function& caller,
                            const ir::Function& callee) {
  if (!callee.native_symbol.empty() ||
      !std::cmp_equal(callee.parameter_count, instruction.immediate) ||
      !IsSameType(caller.value_types[instruction.dst], callee.return_type)) {
    return false;
  }
  for (std::size_t argument = 1; argument < callee.parameter_count; ++argument) {
    if (!IsSameType(caller.value_types[instruction.lhs + argument], callee.value_types[argument])) {
      return false;
    }
  }
  return true;
}

void PlanVirtualCalls(const ir::Function& function,
                      const ir::Program& program,
                      std::span<const std::optional<std::uint32_t>> unique,
                      const absl::flat_hash_map<std::string_view, const ir::Class*>& classes,
                      NativeDebugging debugging,
                      std::vector<std::optional<CallableTarget>>& targets) {
  const auto reloadable = program.native_mode == NativeProgramMode::Reloadable;
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions[index];
    if (instruction.opcode != Opcode::CallVirtual) {
      continue;
    }
    const auto& receiver = function.value_types[instruction.lhs];
    if (receiver.nullable) {
      continue;
    }
    std::optional<std::uint32_t> target;
    if (reloadable) {
      const auto owner = classes.find(receiver.name);
      if (owner != classes.end()) {
        const auto selected = owner->second->dispatch_targets[instruction.index];
        if (selected != kInvalidFunctionTarget) {
          target = selected;
        }
      }
    } else {
      target = unique[instruction.index];
    }
    if (!target) {
      continue;
    }
    const auto& callee = program.functions[*target];
    if (!CanUseVirtualArguments(instruction, function, callee)) {
      continue;
    }
    if (reloadable && (!CanUseScalarFieldBody(callee, program.native_mode, debugging) ||
                       !IsSameType(receiver, callee.value_types.front()))) {
      continue;
    }
    if (targets.empty()) {
      targets.resize(function.instructions.size());
    }
    // 可重载目标只供规范接收者的字段体使用；布局失配在原 CallVirtual 位置恢复。
    targets[index] = CallableTarget{.function = *target};
  }
}

std::vector<std::size_t> CallFinishOrder(const std::vector<std::vector<std::size_t>>& edges) {
  const auto count = edges.size();
  struct Visit final {
    std::size_t function = 0;
    std::size_t next = 0;
  };
  std::vector<std::uint8_t> visited(count);
  std::vector<std::size_t> order;
  std::vector<Visit> pending;
  // 显式 DFS 栈使大型或深层调用图不依赖宿主 C++ 栈深度。
  for (std::size_t root = 0; root < count; ++root) {
    if (visited.at(root) != 0) {
      continue;
    }
    visited.at(root) = 1;
    pending.push_back({.function = root});
    while (!pending.empty()) {
      auto& visit = pending.back();
      const auto& callees = edges.at(visit.function);
      if (visit.next == callees.size()) {
        order.push_back(visit.function);
        pending.pop_back();
      } else {
        const auto callee = callees.at(visit.next++);
        if (visited.at(callee) == 0) {
          visited.at(callee) = 1;
          pending.push_back({.function = callee});
        }
      }
    }
  }
  return order;
}

std::vector<std::size_t> CallComponents(const std::vector<std::vector<std::size_t>>& edges) {
  const auto count = edges.size();
  std::vector<std::vector<std::size_t>> callers(count);
  for (std::size_t caller = 0; caller < count; ++caller) {
    for (const auto callee : edges.at(caller)) {
      callers.at(callee).push_back(caller);
    }
  }
  const auto order = CallFinishOrder(edges);
  std::vector<std::size_t> components(count, count);
  std::vector<std::size_t> members;
  for (const auto root : std::views::reverse(order)) {
    if (components.at(root) != count) {
      continue;
    }
    components.at(root) = root;
    members.push_back(root);
    while (!members.empty()) {
      const auto function = members.back();
      members.pop_back();
      for (const auto caller : callers.at(function)) {
        if (components.at(caller) == count) {
          components.at(caller) = root;
          members.push_back(caller);
        }
      }
    }
  }
  return components;
}

std::vector<std::size_t> FunctionCallees(const ir::Function& function,
                                         const ir::Program& program,
                                         std::span<const std::optional<CallableTarget>> direct) {
  std::vector<std::size_t> callees;
  if (function.json_decode_plan) {
    for (const auto node : ir::JSONDecodeClosure(program, *function.json_decode_plan)) {
      const auto target = program.json_decode_plans.at(node).constructor;
      if (target != kInvalidFunctionTarget) {
        callees.push_back(target);
      }
    }
  }
  ForEachDirectCallee(function, direct, [&](std::size_t callee) { callees.push_back(callee); });
  SortUnique(callees);
  return callees;
}

std::vector<std::size_t> RepeatedCallees(const ir::Function& function,
                                         std::span<const std::optional<CallableTarget>> direct) {
  const auto& instructions = function.instructions;
  std::vector<std::int64_t> boundaries(instructions.size() + 1);
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const auto& instruction = instructions[index];
    if (ir::IsBackJump(instruction, index)) {
      ++boundaries[instruction.index];
      --boundaries[index + 1];
    }
  }
  std::vector<std::size_t> callees;
  std::int64_t loops = 0;
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    loops += boundaries[index];
    if (loops == 0) {
      continue;
    }
    const auto& instruction = instructions[index];
    std::optional<std::size_t> target;
    if (instruction.opcode == Opcode::Call) {
      target = instruction.index;
    } else if (!direct.empty()) {
      const auto& callable = direct[index];
      if (callable) {
        target = callable->function;
      }
    }
    if (!target) {
      continue;
    }
    callees.push_back(*target);
  }
  SortUnique(callees);
  return callees;
}

void PlanReloadableEntrySlots(NativeCallPlan& plan,
                              const ir::Program& program,
                              const std::vector<std::vector<std::size_t>>& edges,
                              NativeDebugging debugging) {
  const auto& functions = program.functions;
  const auto count = functions.size();
  const auto components = CallComponents(edges);
  llvm::SmallVector<std::optional<LeafKind>> leaves(count);
  llvm::SmallVector<std::optional<bool>> scalar_fields(count);
  const auto leaf_kind = [&](std::size_t callee) {
    auto& leaf = leaves[callee];
    if (!leaf) {
      leaf = PlanNonrecursiveLeaf(functions[callee], debugging);
    }
    return *leaf;
  };
  const auto can_use_scalar_fields = [&](std::size_t callee) {
    auto& eligible = scalar_fields[callee];
    if (!eligible) {
      eligible = CanUseScalarFieldBody(functions[callee], program.native_mode, debugging);
    }
    return *eligible;
  };
  for (std::size_t index = 0; index < count; ++index) {
    const auto& callees = edges[index];
    const auto component = components[index];
    const auto is_external = [&](std::size_t callee) {
      return functions[callee].native_symbol.empty() && component != components[callee];
    };
    if (std::ranges::none_of(callees, is_external)) {
      continue;
    }
    auto repeated = RepeatedCallees(functions[index], plan.direct_calls[index]);
    std::erase_if(repeated, [&](std::size_t callee) {
      return leaf_kind(callee) == LeafKind::None && !can_use_scalar_fields(callee);
    });
    for (const auto callee : callees) {
      const auto external = is_external(callee);
      const auto& target = functions[callee];
      const auto constructor_leaf =
          external && target.is_generated_constructor && leaf_kind(callee) != LeafKind::None;
      // 循环内的小型纯计算和接收者标量字段方法需要内联；完整函数体保留布局守卫、
      // 安全点和错误前缀，其函数体依赖使调用者随编辑失效并重新编译。
      // 空生成构造仍保留栈预算和可观察入口；其他冷调用使用槽，限制重载扇出。
      if (external && !constructor_leaf && !std::ranges::binary_search(repeated, callee)) {
        plan.slot_calls[index].push_back(callee);
      }
    }
  }
}

void RemoveSlottedVirtualCalls(NativeCallPlan& plan,
                               const ir::Program& program,
                               std::vector<std::vector<std::size_t>>& edges) {
  for (std::size_t index = 0; index < program.functions.size(); ++index) {
    auto& direct = plan.direct_calls[index];
    const auto& function = program.functions[index];
    const auto& instructions = function.instructions;
    for (std::size_t instruction = 0; instruction < direct.size(); ++instruction) {
      auto& target = direct[instruction];
      if (instructions[instruction].opcode == Opcode::CallVirtual && target &&
          plan.IsEntrySlotRequired({.caller = index, .callee = target->function})) {
        target.reset();
      }
    }
    edges[index] = FunctionCallees(function, program, direct);
    std::erase_if(plan.slot_calls[index], [&](std::size_t callee) {
      return !std::ranges::binary_search(edges[index], callee);
    });
  }
}

}  // namespace

void ForEachDirectCallee(const ir::Function& function,
                         std::span<const std::optional<CallableTarget>> direct,
                         llvm::function_ref<void(std::size_t)> visit) {
  for (const auto& instruction : function.instructions) {
    if (instruction.opcode == Opcode::Call) {
      visit(instruction.index);
    }
  }
  for (const auto& target : direct) {
    if (target) {
      visit(target->function);
    }
  }
}

bool IsNamedBaselineCall(const ir::Function& callee, NativeProgramMode mode) {
  return mode == NativeProgramMode::Reloadable && callee.native_symbol.empty() &&
         callee.identity.local == 0 && callee.identity.declaration != DeclarationId{};
}

bool NativeCallPlan::IsEntrySlotRequired(CallEdge edge) const {
  return std::ranges::binary_search(slot_calls.at(edge.caller), edge.callee);
}

NativeCallPlan PlanNativeCalls(const ir::Program& program,
                               NativeCodeTier tier,
                               NativeDebugging debugging) {
  const auto& functions = program.functions;
  const auto count = functions.size();
  NativeCallPlan plan{.slot_calls = {}, .dependencies = {}, .direct_calls = {}};
  plan.slot_calls.resize(count);
  plan.direct_calls.resize(count);
  const auto reloadable = program.native_mode == NativeProgramMode::Reloadable;
  const auto unique = !reloadable && tier == NativeCodeTier::Optimized
                          ? FindUniqueVirtualTargets(program)
                          : std::vector<std::optional<std::uint32_t>>{};
  absl::flat_hash_map<std::string_view, const ir::Class*> classes;
  if (reloadable && tier == NativeCodeTier::Optimized) {
    for (const auto& owner : program.classes) {
      classes.emplace(owner.name, &owner);
    }
  }
  std::vector<std::vector<std::size_t>> edges(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto& function = functions[index];
    auto& direct = plan.direct_calls[index];
    if (tier == NativeCodeTier::Optimized) {
      direct = FindDirectCalls(function, program);
      PlanVirtualCalls(function, program, unique, classes, debugging, direct);
    }
    edges[index] = FunctionCallees(function, program, direct);
  }
  if (reloadable && tier == NativeCodeTier::Optimized) {
    PlanReloadableEntrySlots(plan, program, edges, debugging);
    RemoveSlottedVirtualCalls(plan, program, edges);
  }
  plan.dependencies = std::move(edges);
  for (std::size_t index = 0; index < count; ++index) {
    std::erase_if(plan.dependencies.at(index), [&](std::size_t callee) {
      // 恢复入口仍可能直接引用版本内闭包；这些边也必须保活并参与失效。
      if (!IsNamedBaselineCall(functions.at(callee), program.native_mode)) {
        return false;
      }
      return tier == NativeCodeTier::Baseline ||
             plan.IsEntrySlotRequired({.caller = index, .callee = callee});
    });
  }
  return plan;
}

std::vector<std::optional<std::uint32_t>> FindUniqueVirtualTargets(const ir::Program& program) {
  std::vector<std::optional<std::uint32_t>> targets(program.selector_declarations.size());
  std::vector<std::uint8_t> polymorphic(targets.size());
  for (const auto& type : program.classes) {
    for (std::size_t selector = 0; selector < type.dispatch_targets.size(); ++selector) {
      const auto target = type.dispatch_targets[selector];
      if (target == kInvalidFunctionTarget || polymorphic[selector] != 0) {
        continue;
      }
      auto& selected = targets[selector];
      if (selected && *selected != target) {
        selected.reset();
        polymorphic[selector] = 1;
      } else {
        selected = target;
      }
    }
  }
  return targets;
}

bool CanInlineSmallFunction(const ir::Function& function) {
  // 以源操作限制展开规模；字段访问的初始化、迁移和失败分支仍随函数体一起内联。
  // 这些冷分支不应阻止几个标量或字段操作与调用方的拆包、结果编码合并。
  if (!ir::CanAnalyzeSynchronousFunction(function) ||
      function.parameter_count > kMaximumInlineInstructions ||
      function.instructions.size() > kMaximumInlineInstructions) {
    return false;
  }
  return std::ranges::all_of(function.instructions, [](const ir::Instruction& instruction) {
    return instruction.opcode == Opcode::Return || instruction.opcode == Opcode::GetField ||
           instruction.opcode == Opcode::SetField ||
           !ir::AnalyzeEffects(instruction).MayCollectOnSuccess();
  });
}

bool CanUseScalarFieldBody(const ir::Function& function,
                           NativeProgramMode mode,
                           NativeDebugging debugging) {
  if (debugging != NativeDebugging::Safepoints || !CanInlineSmallFunction(function) ||
      !function.signature || function.signature->hidden_parameters != 1 ||
      !function.signature->receiver || function.value_types[0].kind != Kind::Class ||
      !IsScalar(function.return_type)) {
    return false;
  }
  const auto parameters =
      std::span(function.value_types).first(function.parameter_count).subspan(1);
  if (!std::ranges::all_of(parameters, IsScalar)) {
    return false;
  }
  const auto accesses = ir::AnalyzeRegisterAccesses(function, {});
  const auto flow = ir::AnalyzeRegisterFlow(function, accesses);
  bool fields = false;
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions[index];
    if (ir::IsBackJump(instruction, index) || instruction.construction ||
        instruction.completes_construction) {
      return false;
    }
    if (instruction.opcode == Opcode::Return) {
      continue;
    }
    if (instruction.opcode == Opcode::GetField || instruction.opcode == Opcode::SetField) {
      const auto reading = instruction.opcode == Opcode::GetField;
      const auto value = reading ? instruction.dst : instruction.lhs;
      const auto receiver = reading ? instruction.lhs : instruction.dst;
      if (instruction.field_declaration.has_value() != (mode == NativeProgramMode::Reloadable) ||
          !IsScalar(function.value_types[value]) || flow.origins[receiver] != 0) {
        return false;
      }
      fields = true;
      continue;
    }
    const auto effects = ir::AnalyzeEffects(instruction);
    if (effects.MayCollectOnSuccess() || effects.reads_state || effects.writes_state ||
        effects.may_suspend) {
      return false;
    }
  }
  // 仅接收者的标量字段允许副作用；封闭布局与可重载布局各自由对应入口建立访问契约。
  return fields;
}

LeafKind PlanNonrecursiveLeaf(const ir::Function& function, NativeDebugging debugging) {
  constexpr std::size_t kMaximumKernelInstructions = 128;
  const auto& instructions = function.instructions;
  const auto count = instructions.size();
  const auto parameters = function.parameter_count;
  if (debugging != NativeDebugging::Safepoints || !ir::CanAnalyzeSynchronousFunction(function) ||
      !IsScalar(function.return_type) || parameters > kMaximumKernelInstructions ||
      count > kMaximumKernelInstructions) {
    return LeafKind::None;
  }
  bool bounded = parameters <= kMaximumInlineInstructions && count <= kMaximumInlineInstructions;
  for (std::size_t index = 0; index < count; ++index) {
    const auto& instruction = instructions[index];
    if (instruction.opcode == Opcode::Return) {
      continue;
    }
    const auto effects = ir::AnalyzeEffects(instruction);
    const auto array_length = ir::IsBuiltinCall(instruction, Builtin::ArrayLength);
    if (effects.MayCollectOnSuccess() || effects.writes_state ||
        (effects.reads_state && instruction.opcode != Opcode::GetIndex && !array_length)) {
      return LeafKind::None;
    }
    bounded = bounded && !effects.reads_state && !ir::IsBackJump(instruction, index);
    const auto access = ir::AnalyzeRegisters(instruction, {});
    bounded =
        bounded && (!access.defines_value || IsScalar(function.value_types.at(instruction.dst)));
    bool guarded = true;
    ir::ForEachUse(instruction, access, [&](ir::RegisterId reg) {
      const auto& type = function.value_types.at(reg);
      const auto scalar = IsScalar(type);
      bounded = bounded && scalar;
      guarded = guarded && (scalar || instruction.opcode == Opcode::Move ||
                            (reg == instruction.lhs && type.kind == Kind::Array &&
                             (instruction.opcode == Opcode::GetIndex || array_length)));
    });
    if (!guarded) {
      return LeafKind::None;
    }
  }
  return bounded ? LeafKind::Bounded : LeafKind::Guarded;
}

std::vector<std::optional<CallableTarget>> FindDirectCalls(const ir::Function& function,
                                                           const ir::Program& program) {
  if (std::ranges::none_of(function.instructions, [](const auto& instruction) {
        return instruction.opcode == Opcode::CallIndirect;
      })) {
    return {};
  }
  const auto accesses = ir::AnalyzeRegisterAccesses(function, program.functions);
  const auto register_flow = ir::AnalyzeRegisterFlow(function, accesses);
  std::vector<std::optional<CallableTarget>> targets(function.instructions.size());
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions.at(index);
    if (instruction.opcode != Opcode::CallIndirect) {
      continue;
    }
    const auto definition = register_flow.OriginDefinition(instruction.rhs);
    if (!definition) {
      continue;
    }
    const auto& source = function.instructions[*definition];
    const auto target = source.opcode == Opcode::NewInstance
                            ? program.classes[source.index].callable
                            : std::nullopt;
    if (!target || target->mode == CallableMode::VirtualMethod) {
      continue;
    }
    const auto& callee = program.functions.at(target->function);
    if (CanCallDirectly(instruction, function, callee, target->mode)) {
      targets.at(index) = target;
    }
  }
  return targets;
}

}  // namespace vela::compiler::native
