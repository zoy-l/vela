#include "compiler/aot/native_call_optimizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "base/callable.h"
#include "base/opcode.h"
#include "compiler/ir/ir.h"
#include "compiler/native/call_targets.h"

namespace vela::compiler::aot {
namespace {

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
  for (auto& function : program.functions) {
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
    for (auto& instruction : code) {
      if (instruction.opcode == Opcode::Jump || instruction.opcode == Opcode::JumpIfFalse ||
          instruction.opcode == Opcode::JumpIfNull) {
        instruction.index = offsets.at(instruction.index);
      }
    }
    for (auto& handler : function.handlers) {
      handler.target = offsets.at(handler.target);
    }
    function.instructions = std::move(code);
  }
}

}  // namespace vela::compiler::aot
