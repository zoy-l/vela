#include "compiler/native/call_targets.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include "base/callable.h"
#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {
namespace {

class CallableDefinitions final {
 public:
  CallableDefinitions(const ir::Function& function, const ir::Program& program)
      : function_(&function),
        program_(&program),
        counts_(function.register_count),
        definitions_(function.register_count),
        states_(function.register_count),
        classes_(function.register_count) {
    for (std::size_t reg = 0; reg < function.parameter_count; ++reg) {
      counts_.at(reg) = 1;
    }
    for (std::size_t index = 0; index < function.instructions.size(); ++index) {
      const auto& instruction = function.instructions.at(index);
      if (ir::AnalyzeRegisters(instruction, program.functions).defines_value) {
        ++counts_.at(instruction.dst);
        definitions_.at(instruction.dst) = index;
      }
    }
  }

  std::optional<ir::ClassId> Resolve(ir::RegisterId reg) {
    const auto& function = *function_;
    const auto& program = *program_;
    std::vector<ir::RegisterId> path;
    std::optional<ir::ClassId> result;
    while (states_.at(reg) == 0) {
      states_.at(reg) = 1;
      path.push_back(reg);
      const auto definition = definitions_.at(reg);
      if (counts_.at(reg) != 1 || !definition) {
        break;
      }
      const auto& instruction = function.instructions.at(*definition);
      if (instruction.opcode == Opcode::Move) {
        reg = instruction.lhs;
        continue;
      }
      if (instruction.opcode == Opcode::NewInstance &&
          program.classes.at(instruction.index).callable) {
        result = instruction.index;
      }
      break;
    }
    if (states_.at(reg) == 2) {
      result = classes_.at(reg);
    }
    for (const auto item : path) {
      classes_.at(item) = result;
      states_.at(item) = 2;
    }
    return result;
  }

 private:
  const ir::Function* function_;
  const ir::Program* program_;
  std::vector<std::size_t> counts_;
  std::vector<std::optional<std::size_t>> definitions_;
  std::vector<std::uint8_t> states_;
  std::vector<std::optional<ir::ClassId>> classes_;
};

bool IsSameType(const frontend::TypeDescriptor& left, const frontend::TypeDescriptor& right) {
  return left.kind == right.kind && left.nullable == right.nullable && left.name == right.name;
}

bool CanCallDirectly(const ir::Instruction& instruction,
                     const ir::Function& caller,
                     const ir::Function& callee,
                     CallableMode mode) {
  if (mode != CallableMode::Plain && mode != CallableMode::Closure) {
    return false;
  }
  const auto hidden = mode == CallableMode::Closure ? 1U : 0U;
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

}  // namespace

bool NativeCallPlan::UsesEntrySlot(CallEdge edge) const {
  return slot_targets.at(edge.callee) != 0 &&
         components.at(edge.caller) != components.at(edge.callee);
}

NativeCallPlan PlanNativeCalls(const ir::Program& program, NativeCodeTier tier) {
  const auto& functions = program.functions;
  const auto count = functions.size();
  std::vector<std::vector<std::size_t>> edges(count);
  for (std::size_t index = 0; index < count; ++index) {
    auto& callees = edges.at(index);
    const auto& function = functions.at(index);
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::Call) {
        callees.push_back(instruction.index);
      }
    }
    if (tier == NativeCodeTier::Optimized) {
      for (const auto& target : FindDirectCalls(function, program)) {
        if (target) {
          callees.push_back(target->function);
        }
      }
    }
    std::ranges::sort(callees);
    const auto duplicates = std::ranges::unique(callees);
    callees.erase(duplicates.begin(), duplicates.end());
  }
  NativeCallPlan plan{.components = CallComponents(edges), .slot_targets = {}, .dependencies = {}};
  const auto reloadable = program.native_mode == NativeProgramMode::Reloadable;
  for (const auto& function : functions) {
    plan.slot_targets.push_back(
        reloadable && tier == NativeCodeTier::Optimized && function.native_symbol.empty() ? 1 : 0);
  }
  plan.dependencies = std::move(edges);
  for (std::size_t index = 0; index < count; ++index) {
    std::erase_if(plan.dependencies.at(index), [&](std::size_t callee) {
      // 恢复入口仍可能直接引用版本内闭包；这些边也必须保活并参与失效。
      if (!IsNamedBaselineCall(functions.at(callee), program.native_mode)) {
        return false;
      }
      return tier == NativeCodeTier::Baseline ||
             plan.UsesEntrySlot({.caller = index, .callee = callee});
    });
  }
  return plan;
}

std::vector<std::optional<CallableTarget>> FindDirectCalls(const ir::Function& function,
                                                           const ir::Program& program) {
  CallableDefinitions definitions(function, program);
  std::vector<std::optional<CallableTarget>> targets(function.instructions.size());
  for (std::size_t index = 0; index < function.instructions.size(); ++index) {
    const auto& instruction = function.instructions.at(index);
    if (instruction.opcode != Opcode::CallIndirect) {
      continue;
    }
    const auto type = definitions.Resolve(instruction.rhs);
    if (!type) {
      continue;
    }
    const auto target = *program.classes.at(*type).callable;
    const auto& callee = program.functions.at(target.function);
    if (CanCallDirectly(instruction, function, callee, target.mode)) {
      targets.at(index) = target;
    }
  }
  return targets;
}

}  // namespace vela::compiler::native
