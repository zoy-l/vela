#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

RegisterId Lowerer::LowerConstantLabel(const frontend::Expression& source) {
  const auto& constant = model_.case_values.at(&source);
  const auto label = AllocateTemporary(source.location);
  if (const auto* integer = std::get_if<std::int64_t>(&constant)) {
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = label,
        .immediate = *integer,
        .location = source.location,
    });
  } else if (const auto* boolean = std::get_if<bool>(&constant)) {
    Emit({
        .opcode = Opcode::LoadBool,
        .dst = label,
        .immediate = *boolean ? 1 : 0,
        .location = source.location,
    });
  } else {
    frontend::Expression literal;
    literal.kind = frontend::Expression::Kind::StringLiteral;
    literal.location = source.location;
    literal.text = std::get<std::string>(constant);
    return LowerStringLiteral(literal).reg;
  }
  return label;
}

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
    const auto label = LowerConstantLabel(*branch.label);
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
  BeginControl(ControlTargets::Kind::Switch);
  for (std::size_t index = 0; index < statement.cases.size(); ++index) {
    const auto target = static_cast<std::uint32_t>(current_function_->instructions.size());
    if (const auto entry = entries.at(index)) {
      current_function_->instructions.at(*entry).index = target;
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
  FinishControl(std::nullopt);
}

}  // namespace vela::ir
