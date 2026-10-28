#include "compiler/aot/native_call_optimizer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "base/callable.h"
#include "base/opcode.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/analysis.h"

namespace vela::compiler::aot {
namespace {

class CallableDefinitions final {
 public:
  CallableDefinitions(const ir::Function& function, const ir::Program& program)
      : function_(function),
        program_(program),
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
    std::vector<ir::RegisterId> path;
    std::optional<ir::ClassId> result;
    while (states_.at(reg) == 0) {
      states_.at(reg) = 1;
      path.push_back(reg);
      const auto definition = definitions_.at(reg);
      if (counts_.at(reg) != 1 || !definition) {
        break;
      }
      const auto& instruction = function_.instructions.at(*definition);
      if (instruction.opcode == Opcode::Move) {
        reg = instruction.lhs;
        continue;
      }
      if (instruction.opcode == Opcode::NewInstance &&
          program_.classes.at(instruction.index).callable) {
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
  const ir::Function& function_;
  const ir::Program& program_;
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

}  // namespace

void DevirtualizeNativeCalls(ir::Program& program) {
  for (auto& function : program.functions) {
    CallableDefinitions definitions(function, program);
    std::vector<std::optional<CallableTarget>> targets(function.instructions.size());
    bool changed = false;
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
        changed = true;
      }
    }
    if (!changed) {
      continue;
    }
    std::vector<ir::Instruction> code;
    std::vector<std::uint32_t> offsets(function.instructions.size() + 1);
    for (std::size_t index = 0; index < function.instructions.size(); ++index) {
      offsets.at(index) = static_cast<std::uint32_t>(code.size());
      auto instruction = function.instructions.at(index);
      const auto target = targets.at(index);
      if (target) {
        if (target->mode == CallableMode::Closure) {
          const auto& callee = program.functions.at(target->function);
          const auto base = function.register_count;
          for (std::size_t parameter = 0; parameter < callee.parameter_count; ++parameter) {
            const auto source = parameter == 0 ? instruction.rhs : instruction.lhs + parameter - 1;
            const auto type =
                parameter == 0 ? callee.value_types.front() : function.value_types.at(source);
            code.push_back({.opcode = Opcode::Move,
                            .dst = function.register_count++,
                            .lhs = static_cast<ir::RegisterId>(source),
                            .location = instruction.location,
                            .handler = instruction.handler});
            function.value_types.push_back(type);
          }
          instruction.lhs = base;
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
