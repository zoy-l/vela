#include "compiler/native/call_targets.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

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

bool IsSameType(const frontend::TypeDescriptor& left, const frontend::TypeDescriptor& right) {
  return left.kind == right.kind && left.nullable == right.nullable && left.name == right.name;
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

bool IsNamedBaselineCall(const ir::Function& callee, NativeProgramMode mode) {
  return mode == NativeProgramMode::Reloadable && callee.native_symbol.empty() &&
         callee.identity.local == 0 && callee.identity.declaration != DeclarationId{};
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
  for (const auto& instruction : function.instructions) {
    if (instruction.opcode == Opcode::Call) {
      callees.push_back(instruction.index);
    }
  }
  for (const auto& target : direct) {
    if (target) {
      callees.push_back(target->function);
    }
  }
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
  const auto leaf_kind = [&](std::size_t callee) {
    auto& leaf = leaves[callee];
    if (!leaf) {
      leaf = PlanNonrecursiveLeaf(functions[callee], debugging);
    }
    return *leaf;
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
    std::erase_if(repeated,
                  [&](std::size_t callee) { return leaf_kind(callee) == LeafKind::None; });
    for (const auto callee : callees) {
      const auto external = is_external(callee);
      const auto& target = functions[callee];
      const auto constructor_leaf =
          external && target.is_generated_constructor && leaf_kind(callee) != LeafKind::None;
      // 循环内的小型纯计算需要内联；其函数体依赖使调用者随编辑失效并重新编译。
      // 空生成构造仍保留栈预算和可观察入口；其他冷调用使用槽，限制重载扇出。
      if (external && !constructor_leaf && !std::ranges::binary_search(repeated, callee)) {
        plan.slot_calls[index].push_back(callee);
      }
    }
  }
}

}  // namespace

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
  std::vector<std::vector<std::size_t>> edges(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto& function = functions[index];
    auto& direct = plan.direct_calls[index];
    if (tier == NativeCodeTier::Optimized) {
      direct = FindDirectCalls(function, program);
    }
    edges[index] = FunctionCallees(function, program, direct);
  }
  const auto reloadable = program.native_mode == NativeProgramMode::Reloadable;
  if (reloadable && tier == NativeCodeTier::Optimized) {
    PlanReloadableEntrySlots(plan, program, edges, debugging);
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

bool CanInlineSmallFunction(const ir::Function& function) {
  // 以源操作限制展开规模；安全点、溢出失败和恢复分支不应让几个标量操作失去内联。
  if (!ir::CanAnalyzeSynchronousFunction(function) ||
      function.parameter_count > kMaximumInlineInstructions ||
      function.instructions.size() > kMaximumInlineInstructions) {
    return false;
  }
  return std::ranges::all_of(function.instructions, [](const ir::Instruction& instruction) {
    return instruction.opcode == Opcode::Return ||
           !ir::AnalyzeEffects(instruction).MayCollectOnSuccess();
  });
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
    if (!target) {
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
