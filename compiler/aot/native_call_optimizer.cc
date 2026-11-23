#include "compiler/aot/native_call_optimizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "base/callable.h"
#include "base/native_program_mode.h"
#include "base/opcode.h"
#include "base/reflection.h"
#include "compiler/ir/analysis.h"
#include "compiler/ir/ir.h"
#include "compiler/native/call_targets.h"

namespace vela::compiler::aot {
namespace {

void DevirtualizeInstanceCalls(ir::Function& function,
                               const ir::Program& program,
                               std::span<const std::optional<std::uint32_t>> unique) {
  if (std::ranges::none_of(function.instructions, [](const auto& instruction) {
        return instruction.opcode == Opcode::CallVirtual;
      })) {
    return;
  }
  const auto accesses = ir::AnalyzeRegisterAccesses(function, program.functions);
  const auto flow = ir::AnalyzeRegisterFlow(function, accesses);
  for (auto& instruction : function.instructions) {
    if (instruction.opcode != Opcode::CallVirtual) {
      continue;
    }
    auto selected = unique[instruction.index];
    const auto definition = flow.OriginDefinition(instruction.lhs);
    if (definition) {
      const auto& source = function.instructions[*definition];
      if (source.opcode == Opcode::NewInstance) {
        const auto target = program.classes[source.index].dispatch_targets.at(instruction.index);
        if (target != kInvalidFunctionTarget) {
          selected = target;
        }
      }
    }
    if (selected) {
      instruction.opcode = Opcode::Call;
      instruction.index = *selected;
    }
  }
}

void BindClosureArguments(ir::Function& function,
                          const ir::Function& callee,
                          ir::Instruction& instruction,
                          CallableMode mode,
                          std::vector<ir::Instruction>& code) {
  const auto base = function.register_count;
  for (std::size_t parameter = 0; parameter < callee.parameter_count; ++parameter) {
    const auto source = parameter == 0 ? instruction.rhs : instruction.lhs + parameter - 1;
    const auto type = parameter == 0 ? callee.value_types.front() : function.value_types.at(source);
    code.push_back({
        .opcode =
            parameter == 0 && mode == CallableMode::BoundMethod ? Opcode::GetField : Opcode::Move,
        .dst = function.register_count++,
        .lhs = static_cast<ir::RegisterId>(source),
        .location = instruction.location,
        .handler = instruction.handler,
    });
    function.value_types.push_back(type);
  }
  instruction.lhs = base;
}

}  // namespace

void DevirtualizeNativeCalls(ir::Program& program) {
  if (program.native_mode != NativeProgramMode::Closed) {
    return;
  }
  const auto unique = native::FindUniqueVirtualTargets(program);
  for (auto& member : program.reflection_members) {
    if ((member.flags & reflection::kVirtual) != 0) {
      if (const auto target = unique[member.target]) {
        member.target = *target;
        member.flags &= ~reflection::kVirtual;
      }
    }
  }
  for (auto& function : program.functions) {
    DevirtualizeInstanceCalls(function, program, unique);
    const auto targets = native::FindDirectCalls(function, program);
    if (std::ranges::none_of(targets, [](const auto& target) { return target.has_value(); })) {
      continue;
    }
    std::vector<ir::Instruction> code;
    std::vector<std::uint32_t> offsets(function.instructions.size() + 1);
    for (std::size_t index = 0; index < function.instructions.size(); ++index) {
      offsets.at(index) = static_cast<std::uint32_t>(code.size());
      auto instruction = function.instructions.at(index);
      const auto target = targets.at(index);
      if (target) {
        if (target->mode == CallableMode::Closure || target->mode == CallableMode::BoundMethod) {
          const auto& callee = program.functions.at(target->function);
          BindClosureArguments(function, callee, instruction, target->mode, code);
        }
        // 只追踪单定义和 Move 链，所有可能覆写都使目标未知；对象的 callable target 不可变。
        // NewInstance 在当前实例建立环境，因此直接调用保留捕获对象且无需跨实例重绑定。
        instruction.opcode = Opcode::Call;
        instruction.rhs = 0;
        instruction.index = target->function;
      }
      code.push_back(instruction);
    }
    offsets.back() = static_cast<std::uint32_t>(code.size());
    function.instructions = std::move(code);
    ir::RemapJumpTargets(function, offsets);
  }
}

}  // namespace vela::compiler::aot
