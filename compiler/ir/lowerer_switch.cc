#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

RegisterId Lowerer::LowerConstantLabel(const frontend::Expression& source) {
  const auto& constant = model_.case_values.at(&source);
  const auto label = AllocateRegister(source.location);
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
    return EmitStringConstant(std::get<std::string>(constant), source.location).reg;
  }
  return label;
}

std::size_t Lowerer::LowerLabelMismatch(const LoweredValue& selector,
                                        RegisterId label,
                                        frontend::SourceLocation location) {
  const auto equal = AllocateRegister(location);
  Emit({
      .opcode = SelectEqualityOpcode(selector.type),
      .dst = equal,
      .lhs = selector.reg,
      .rhs = label,
      .location = location,
  });
  const auto mismatch = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = equal, .location = location});
  return mismatch;
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
    const auto mismatch = LowerLabelMismatch(value, label, branch.location);
    entries.at(index) = current_function_->instructions.size();
    Emit({.opcode = Opcode::Jump, .location = branch.location});
    BindJump(mismatch);
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
