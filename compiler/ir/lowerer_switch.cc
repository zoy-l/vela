#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

void Lowerer::LowerSwitch(const frontend::Statement& statement) {
  const auto value = LowerExpression(*statement.condition);
  std::vector<std::optional<std::size_t>> entries(statement.cases.size());
  std::optional<std::size_t> default_branch;
  for (std::size_t index = 0; index < statement.cases.size(); ++index) {
    const auto& branch = statement.cases.at(index);
    if (!branch.label) {
      default_branch = index;
      continue;
    }
    const auto& constant = model_.case_values.at(branch.label.get());
    const auto label = AllocateTemporary(branch.location);
    if (const auto* integer = std::get_if<std::int64_t>(&constant)) {
      Emit({
          .opcode = Opcode::LoadInt,
          .dst = label,
          .immediate = *integer,
          .location = branch.location,
      });
    } else if (const auto* boolean = std::get_if<bool>(&constant)) {
      Emit({
          .opcode = Opcode::LoadBool,
          .dst = label,
          .immediate = *boolean ? 1 : 0,
          .location = branch.location,
      });
    } else {
      const auto string_id = static_cast<std::uint32_t>(program_.strings.size());
      program_.strings.push_back(std::get<std::string>(constant));
      Emit({
          .opcode = Opcode::LoadString,
          .dst = label,
          .index = string_id,
          .location = branch.location,
      });
    }
    const auto equal = AllocateTemporary(branch.location);
    const auto kind = value.type.kind;
    auto opcode = Opcode::EqualInt;
    if (kind == frontend::TypeDescriptor::Kind::String) {
      opcode = Opcode::EqualString;
    } else if (kind == frontend::TypeDescriptor::Kind::Bool) {
      opcode = Opcode::EqualBool;
    }
    Emit({
        .opcode = opcode,
        .dst = equal,
        .lhs = value.reg,
        .rhs = label,
        .location = branch.location,
    });
    const auto mismatch = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfFalse, .lhs = equal, .location = branch.location});
    entries.at(index) = current_function_->instructions.size();
    Emit({.opcode = Opcode::Jump, .location = branch.location});
    current_function_->instructions.at(mismatch).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
  }
  const auto unmatched = current_function_->instructions.size();
  Emit({.opcode = Opcode::Jump, .location = statement.location});
  controls_.push_back(
      {.kind = ControlTargets::Kind::Switch, .break_jumps = {}, .continue_jumps = {}});
  for (std::size_t index = 0; index < statement.cases.size(); ++index) {
    const auto target = static_cast<std::uint32_t>(current_function_->instructions.size());
    if (entries.at(index)) {
      current_function_->instructions.at(*entries.at(index)).index = target;
    }
    if (default_branch == index) {
      current_function_->instructions.at(unmatched).index = target;
    }
    LowerBlock(*statement.cases.at(index).body);
  }
  const auto end = static_cast<std::uint32_t>(current_function_->instructions.size());
  if (!default_branch) {
    current_function_->instructions.at(unmatched).index = end;
  }
  for (const auto jump : controls_.back().break_jumps) {
    current_function_->instructions.at(jump).index = end;
  }
  controls_.pop_back();
}

}  // namespace vela::ir
